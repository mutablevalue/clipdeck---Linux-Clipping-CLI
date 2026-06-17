#include "gstreamer_recorder.hpp"

#include "../utils/logger.hpp"
#include "../utils/runtime_paths.hpp"
#include "segment_file.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <utility>

#if defined(CLIPDECK_HAS_GSTREAMER)
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gst/gst.h>
#include <glib-object.h>
#endif

namespace {

constexpr std::string_view kRecorderContext = "native-recorder";
constexpr int kSegmentSeconds = 1;
constexpr std::uint32_t kPortalMonitorSource = 1;
constexpr std::uint32_t kPortalWindowSource = 2;

struct RecorderSegmentSummary {
  std::size_t count = 0;
  std::chrono::milliseconds duration{0};
  std::optional<int> audio_sample_rate;
  std::optional<int> audio_channels;
};

std::size_t RetainedSegmentCount(const clipdeck::RecorderConfig &config) {
  return static_cast<std::size_t>(config.clip_length_seconds +
                                  config.buffer_safety_seconds + 3);
}

std::string GstQuote(std::string_view value) {
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      quoted.push_back('\\');
    }
    quoted.push_back(character);
  }
  quoted.push_back('"');
  return quoted;
}

RecorderSegmentSummary SegmentSummary(std::size_t count, bool audio_expected) {
  RecorderSegmentSummary summary{
      .count = count,
      .duration = std::chrono::seconds(count * kSegmentSeconds),
      .audio_sample_rate = std::nullopt,
      .audio_channels = std::nullopt};
  if (audio_expected && count > 0) {
    summary.audio_sample_rate = 48000;
    summary.audio_channels = 2;
  }

  return summary;
}

std::size_t RawVideoFrameBytes(const clipdeck::RecorderConfig &config) {
  return static_cast<std::size_t>(std::max(config.width, 1)) *
         static_cast<std::size_t>(std::max(config.height, 1)) * 3 / 2;
}

int RawVideoQueueFrameLimit(const clipdeck::RecorderConfig &config) {
  return std::clamp(std::max(config.fps / 4, 4), 4, 30);
}

std::size_t EncodedQueueByteLimit(int bitrate_kbps,
                                  std::chrono::seconds duration,
                                  std::size_t minimum_bytes) {
  const auto bitrate_bytes =
      (static_cast<std::size_t>(std::max(bitrate_kbps, 1)) * 1000 *
       static_cast<std::size_t>(duration.count())) /
      8;
  return std::max(bitrate_bytes, minimum_bytes);
}

std::string LiveRawVideoQueue(const clipdeck::RecorderConfig &config) {
  const int frame_limit = RawVideoQueueFrameLimit(config);
  const std::size_t byte_limit =
      RawVideoFrameBytes(config) * static_cast<std::size_t>(frame_limit);
  return std::format("queue max-size-buffers={} max-size-bytes={} "
                     "max-size-time=250000000 leaky=downstream",
                     frame_limit, byte_limit);
}

std::string LiveEncodedVideoQueue(const clipdeck::RecorderConfig &config) {
  return std::format("queue max-size-buffers=0 max-size-bytes={} "
                     "max-size-time=2000000000",
                     EncodedQueueByteLimit(config.video_bitrate_kbps,
                                           std::chrono::seconds(2),
                                           4 * 1024 * 1024));
}

std::string LiveRawAudioQueue() {
  return "queue max-size-buffers=0 max-size-bytes=1048576 "
         "max-size-time=500000000 leaky=downstream";
}

std::string LiveEncodedAudioQueue(const clipdeck::RecorderConfig &config) {
  return std::format("queue max-size-buffers=0 max-size-bytes={} "
                     "max-size-time=2000000000",
                     EncodedQueueByteLimit(config.audio_bitrate_kbps,
                                           std::chrono::seconds(2),
                                           512 * 1024));
}

bool IsSupportedVideoSource(std::string_view source) {
  return source.empty() || source == "portal";
}

std::uint32_t PortalSourceMask() {
  return kPortalMonitorSource | kPortalWindowSource;
}

std::string PortalSourceName(std::uint32_t source_type) {
  if (source_type == kPortalMonitorSource) {
    return "monitor";
  }

  if (source_type == kPortalWindowSource) {
    return "window";
  }

  return "unknown";
}

#if defined(CLIPDECK_HAS_GSTREAMER)
bool GstElementAvailable(std::string_view factory_name) {
  GstElementFactory *factory =
      gst_element_factory_find(std::string(factory_name).c_str());
  if (factory == nullptr) {
    return false;
  }

  gst_object_unref(factory);
  return true;
}

std::string StateChangeReturnName(GstStateChangeReturn state) {
  switch (state) {
  case GST_STATE_CHANGE_FAILURE:
    return "failure";
  case GST_STATE_CHANGE_SUCCESS:
    return "success";
  case GST_STATE_CHANGE_ASYNC:
    return "async";
  case GST_STATE_CHANGE_NO_PREROLL:
    return "no-preroll";
  }

  return "unknown";
}

struct PortalSession {
  GDBusConnection *connection = nullptr;
  int fd = -1;
  std::uint32_t node_id = 0;
  std::uint32_t source_type = 0;
  std::optional<std::string> target_object;
  std::string session_handle;
};

struct PortalResponseState {
  bool done = false;
  std::uint32_t response = 2;
  GVariant *results = nullptr;
};

void OnPortalResponse(GDBusConnection *, const gchar *, const gchar *,
                      const gchar *, const gchar *, GVariant *parameters,
                      gpointer user_data) {
  auto *state = static_cast<PortalResponseState *>(user_data);
  GVariant *results = nullptr;
  guint response = 2;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);

  state->done = true;
  state->response = response;
  state->results = results;
}

std::string PortalToken(std::string_view prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::format("{}{}", prefix, now);
}

std::string PortalSenderPathPart(GDBusConnection *connection) {
  const char *unique_name = g_dbus_connection_get_unique_name(connection);
  if (unique_name == nullptr) {
    return "clipdeck";
  }

  std::string sender = unique_name;
  if (!sender.empty() && sender.front() == ':') {
    sender.erase(sender.begin());
  }

  for (char &character : sender) {
    if (!std::isalnum(static_cast<unsigned char>(character))) {
      character = '_';
    }
  }

  return sender;
}

std::string PortalSessionPath(GDBusConnection *connection,
                              const std::string &session_token) {
  return "/org/freedesktop/portal/desktop/session/" +
         PortalSenderPathPart(connection) + "/" + session_token;
}

std::string PortalRequestPath(GDBusConnection *connection,
                              const std::string &request_token) {
  return "/org/freedesktop/portal/desktop/request/" +
         PortalSenderPathPart(connection) + "/" + request_token;
}

void ClosePortalSession(GDBusConnection *connection, std::string_view session_handle);

std::optional<GVariant *> WaitForPortalResponse(GDBusConnection *connection,
                                                guint subscription,
                                                PortalResponseState &state,
                                                std::string_view action) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  while (!state.done && std::chrono::steady_clock::now() < deadline) {
    g_main_context_iteration(nullptr, FALSE);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  g_dbus_connection_signal_unsubscribe(connection, subscription);

  if (!state.done) {
    Log(LogLevel::Error, kRecorderContext,
        "Timed out waiting for portal " + std::string(action) + " response.");
    return std::nullopt;
  }

  if (state.response != 0) {
    Log(LogLevel::Error, kRecorderContext,
        "Portal " + std::string(action) + " was cancelled or denied.");
    if (state.results != nullptr) {
      g_variant_unref(state.results);
    }
    return std::nullopt;
  }

  return state.results;
}

std::optional<GVariant *> CallPortalRequest(GDBusConnection *connection,
                                            std::string_view method,
                                            GVariant *parameters,
                                            const std::string &request_path,
                                            std::string_view action) {
  PortalResponseState state;
  const guint subscription = g_dbus_connection_signal_subscribe(
      connection, "org.freedesktop.portal.Desktop",
      "org.freedesktop.portal.Request", "Response", request_path.c_str(),
      nullptr, G_DBUS_SIGNAL_FLAGS_NONE, OnPortalResponse, &state, nullptr);

  GError *error = nullptr;
  GVariant *result = g_dbus_connection_call_sync(
      connection, "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast", std::string(method).c_str(),
      parameters, G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
      &error);

  if (result == nullptr) {
    g_dbus_connection_signal_unsubscribe(connection, subscription);
    if (state.results != nullptr) {
      g_variant_unref(state.results);
    }

    const std::string message =
        error == nullptr ? "unknown portal error" : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }

    Log(LogLevel::Error, kRecorderContext,
        "Portal " + std::string(action) + " failed: " + message);
    return std::nullopt;
  }

  const gchar *returned_request_path = nullptr;
  g_variant_get(result, "(&o)", &returned_request_path);
  if (returned_request_path != nullptr &&
      request_path != std::string(returned_request_path)) {
    Log(LogLevel::Warning, kRecorderContext,
        "Portal returned an unexpected request handle for " +
            std::string(action) + ".");
  }
  g_variant_unref(result);

  return WaitForPortalResponse(connection, subscription, state, action);
}

std::optional<PortalSession> OpenPortalScreenCastSession() {
  GError *error = nullptr;
  GDBusConnection *connection =
      g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (connection == nullptr) {
    const std::string message =
        error == nullptr ? "unknown DBus error" : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }
    Log(LogLevel::Error, kRecorderContext,
        "Failed to connect to the session bus for desktop portal: " + message);
    return std::nullopt;
  }

  GVariantBuilder create_options;
  g_variant_builder_init(&create_options, G_VARIANT_TYPE("a{sv}"));
  const auto session_token = PortalToken("clipdecksession");
  const auto create_token = PortalToken("clipdeckcreate");
  g_variant_builder_add(&create_options, "{sv}", "session_handle_token",
                        g_variant_new_string(session_token.c_str()));
  g_variant_builder_add(&create_options, "{sv}", "handle_token",
                        g_variant_new_string(create_token.c_str()));

  auto create_results = CallPortalRequest(
      connection, "CreateSession", g_variant_new("(a{sv})", &create_options),
      PortalRequestPath(connection, create_token), "CreateSession");
  if (!create_results.has_value()) {
    g_object_unref(connection);
    return std::nullopt;
  }

  gchar *session_handle = nullptr;
  if (!g_variant_lookup(create_results.value(), "session_handle", "o",
                        &session_handle) &&
      !g_variant_lookup(create_results.value(), "session_handle", "s",
                        &session_handle)) {
    const auto derived_session_handle =
        PortalSessionPath(connection, session_token);
    Log(LogLevel::Error, kRecorderContext,
        "Portal CreateSession response did not include a session handle; using derived handle " +
            derived_session_handle + ".");
    session_handle = g_strdup(derived_session_handle.c_str());
  }
  g_variant_unref(create_results.value());
  const std::string owned_session_handle = session_handle;

  GVariantBuilder select_options;
  g_variant_builder_init(&select_options, G_VARIANT_TYPE("a{sv}"));
  const auto select_token = PortalToken("clipdeckselect");
  g_variant_builder_add(&select_options, "{sv}", "handle_token",
                        g_variant_new_string(select_token.c_str()));
  g_variant_builder_add(&select_options, "{sv}", "types",
                        g_variant_new_uint32(PortalSourceMask()));
  g_variant_builder_add(&select_options, "{sv}", "multiple",
                        g_variant_new_boolean(FALSE));

  auto select_results = CallPortalRequest(
      connection, "SelectSources",
      g_variant_new("(oa{sv})", session_handle, &select_options),
      PortalRequestPath(connection, select_token), "SelectSources");
  if (!select_results.has_value()) {
    ClosePortalSession(connection, owned_session_handle);
    g_free(session_handle);
    g_object_unref(connection);
    return std::nullopt;
  }
  g_variant_unref(select_results.value());

  GVariantBuilder start_options;
  g_variant_builder_init(&start_options, G_VARIANT_TYPE("a{sv}"));
  const auto start_token = PortalToken("clipdeckstart");
  g_variant_builder_add(&start_options, "{sv}", "handle_token",
                        g_variant_new_string(start_token.c_str()));

  auto start_results = CallPortalRequest(
      connection, "Start",
      g_variant_new("(osa{sv})", session_handle, "", &start_options),
      PortalRequestPath(connection, start_token), "Start");
  if (!start_results.has_value()) {
    ClosePortalSession(connection, owned_session_handle);
    g_free(session_handle);
    g_object_unref(connection);
    return std::nullopt;
  }

  std::uint32_t node_id = 0;
  std::optional<std::uint32_t> source_type;
  std::optional<std::uint64_t> pipewire_serial;
  GVariant *streams =
      g_variant_lookup_value(start_results.value(), "streams",
                             G_VARIANT_TYPE("a(ua{sv})"));
  if (streams != nullptr && g_variant_n_children(streams) > 0) {
    GVariant *stream = g_variant_get_child_value(streams, 0);
    GVariant *properties = nullptr;
    guint stream_node = 0;
    g_variant_get(stream, "(u@a{sv})", &stream_node, &properties);
    node_id = stream_node;
    if (properties != nullptr) {
      guint stream_source_type = 0;
      if (g_variant_lookup(properties, "source_type", "u",
                           &stream_source_type)) {
        source_type = stream_source_type;
      }

      guint64 stream_pipewire_serial = 0;
      if (g_variant_lookup(properties, "pipewire-serial", "t",
                           &stream_pipewire_serial)) {
        pipewire_serial = stream_pipewire_serial;
      }

      g_variant_unref(properties);
    }
    g_variant_unref(stream);
    g_variant_unref(streams);
  }
  g_variant_unref(start_results.value());

  const auto allowed_source_types = PortalSourceMask();
  if (!source_type.has_value() ||
      (source_type.value() & allowed_source_types) == 0) {
    ClosePortalSession(connection, owned_session_handle);
    g_free(session_handle);
    g_object_unref(connection);
    const std::string returned_type =
        source_type.has_value() ? std::to_string(source_type.value())
                                : std::string("<missing>");
    Log(LogLevel::Error, kRecorderContext,
        "Portal returned screen cast source type " + returned_type +
            ", which is not a monitor or window source.");
    return std::nullopt;
  }

  if (node_id == 0 && !pipewire_serial.has_value()) {
    ClosePortalSession(connection, owned_session_handle);
    g_free(session_handle);
    g_object_unref(connection);
    Log(LogLevel::Error, kRecorderContext,
        "Portal Start response did not include a PipeWire stream target.");
    return std::nullopt;
  }

  if (!pipewire_serial.has_value()) {
    Log(LogLevel::Warning, kRecorderContext,
        "Portal Start response did not include a PipeWire stream serial; falling back to the selected monitor node path " +
            std::to_string(node_id) + " on the portal PipeWire remote.");
  }

  GVariantBuilder open_options;
  g_variant_builder_init(&open_options, G_VARIANT_TYPE("a{sv}"));
  GUnixFDList *fd_list = nullptr;
  GVariant *open_result = g_dbus_connection_call_with_unix_fd_list_sync(
      connection, "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast", "OpenPipeWireRemote",
      g_variant_new("(oa{sv})", session_handle, &open_options),
      G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fd_list,
      nullptr, &error);

  g_free(session_handle);

  if (open_result == nullptr) {
    const std::string message =
        error == nullptr ? "unknown portal error" : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }
    Log(LogLevel::Error, kRecorderContext,
        "Portal OpenPipeWireRemote failed: " + message);
    ClosePortalSession(connection, owned_session_handle);
    g_object_unref(connection);
    return std::nullopt;
  }

  gint fd_index = -1;
  g_variant_get(open_result, "(h)", &fd_index);
  g_variant_unref(open_result);

  const int fd = g_unix_fd_list_get(fd_list, fd_index, &error);
  g_object_unref(fd_list);

  if (fd < 0) {
    const std::string message =
        error == nullptr ? "unknown fd-list error" : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }
    Log(LogLevel::Error, kRecorderContext,
        "Failed to read portal PipeWire fd: " + message);
    ClosePortalSession(connection, owned_session_handle);
    g_object_unref(connection);
    return std::nullopt;
  }

  Log(LogLevel::Info, kRecorderContext,
      "Opened desktop portal PipeWire stream node " + std::to_string(node_id) +
          (pipewire_serial.has_value()
               ? " serial " + std::to_string(pipewire_serial.value())
               : std::string()) +
          " for " + PortalSourceName(source_type.value()) + " capture.");
  return PortalSession{.connection = connection,
                       .fd = fd,
                       .node_id = node_id,
                       .source_type = source_type.value(),
                       .target_object =
                           pipewire_serial.has_value()
                               ? std::optional<std::string>{
                                     std::to_string(pipewire_serial.value())}
                               : std::nullopt,
                       .session_handle = owned_session_handle};
}

void ClosePortalSession(GDBusConnection *connection,
                        std::string_view session_handle) {
  if (connection == nullptr || session_handle.empty()) {
    return;
  }

  GError *error = nullptr;
  GVariant *result = g_dbus_connection_call_sync(
      connection, "org.freedesktop.portal.Desktop",
      std::string(session_handle).c_str(), "org.freedesktop.portal.Session",
      "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

  if (result != nullptr) {
    g_variant_unref(result);
    Log(LogLevel::Info, kRecorderContext,
        "Closed desktop portal screen capture session.");
  } else {
    const std::string message =
        error == nullptr ? "unknown portal error" : error->message;
    Log(LogLevel::Warning, kRecorderContext,
        "Failed to close desktop portal session: " + message);
  }

  if (error != nullptr) {
    g_error_free(error);
  }
}

void EnsureGStreamerInitialized() {
  static std::once_flag init_flag;
  std::call_once(init_flag, [] {
    GError *error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
      const std::string message =
          error == nullptr ? "unknown error" : error->message;
      if (error != nullptr) {
        g_error_free(error);
      }

      Log(LogLevel::Error, kRecorderContext,
          "Failed to initialize GStreamer: " + message);
    }
  });
}

std::string PopStartupError(GstBus *bus) {
  if (bus == nullptr) {
    return "GStreamer pipeline failed to enter PLAYING state.";
  }

  GstMessage *message =
      gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND,
                                 static_cast<GstMessageType>(GST_MESSAGE_ERROR |
                                                             GST_MESSAGE_WARNING));
  if (message == nullptr) {
    return "GStreamer pipeline failed to enter PLAYING state.";
  }

  GError *error = nullptr;
  gchar *debug = nullptr;
  std::string text;

  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    gst_message_parse_error(message, &error, &debug);
  } else {
    gst_message_parse_warning(message, &error, &debug);
  }

  text = error == nullptr ? "GStreamer pipeline failed to enter PLAYING state."
                          : error->message;
  if (debug != nullptr && debug[0] != '\0') {
    text += " (";
    text += debug;
    text += ")";
  }

  if (debug != nullptr) {
    g_free(debug);
  }
  if (error != nullptr) {
    g_error_free(error);
  }
  gst_message_unref(message);

  return text;
}
#endif

} // namespace

namespace clipdeck {

GStreamerRecorder::GStreamerRecorder(RecorderConfig config)
    : config_(std::move(config)), muxer_(config_.clip_directory),
      segment_directory_(SegmentDirectory()) {}

GStreamerRecorder::~GStreamerRecorder() { Stop(); }

bool GStreamerRecorder::Start() {
  if (running_) {
    Log(LogLevel::Warning, kRecorderContext,
        "Native recorder is already running.");
    return true;
  }

#if !defined(CLIPDECK_HAS_GSTREAMER)
  SetMessage("GStreamer support was not compiled into this build.", false);
  Log(LogLevel::Error, kRecorderContext, message_);
  return false;
#else
  EnsureGStreamerInitialized();
  CleanSegmentDirectory();

  if (!IsSupportedVideoSource(config_.video_source)) {
    SetMessage(
        "Native recorder supports the portal video source.",
        false);
    Log(LogLevel::Error, kRecorderContext, message_);
    return false;
  }

  const auto portal = OpenPortalScreenCastSession();
  if (!portal.has_value()) {
    SetMessage("Failed to open an XDG desktop portal screen capture session.",
               false);
    return false;
  }

  portal_fd_ = portal->fd;
  portal_node_id_ = portal->node_id;
  capture_source_type_ = PortalSourceName(portal->source_type);
  portal_target_object_ = portal->target_object;
  portal_session_handle_ = portal->session_handle;
  portal_connection_ = portal->connection;
  Log(LogLevel::Info, kRecorderContext,
      portal_target_object_.has_value()
          ? "Using portal PipeWire serial target " +
                portal_target_object_.value() + " for " +
                capture_source_type_ + " capture."
          : "Using portal PipeWire node path " +
                std::to_string(portal_node_id_) + " on the portal remote for " +
                capture_source_type_ + " capture.");

  GError *error = nullptr;
  const std::string pipeline_description = BuildPipelineDescription();
  Log(LogLevel::Debug, kRecorderContext,
      "GStreamer pipeline description: " + pipeline_description);
  GstElement *pipeline = gst_parse_launch(pipeline_description.c_str(), &error);

  if (pipeline == nullptr) {
    const std::string message =
        error == nullptr ? "unknown parse error" : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }

    SetMessage("Failed to build GStreamer pipeline: " + message, false);
    Log(LogLevel::Error, kRecorderContext, message_);
    if (portal_fd_ >= 0) {
      close(portal_fd_);
      portal_fd_ = -1;
      portal_node_id_ = 0;
      capture_source_type_.clear();
      portal_target_object_.reset();
    }
#if defined(CLIPDECK_HAS_GSTREAMER)
    ClosePortalSession(static_cast<GDBusConnection *>(portal_connection_),
                       portal_session_handle_);
    if (portal_connection_ != nullptr) {
      g_object_unref(static_cast<GDBusConnection *>(portal_connection_));
      portal_connection_ = nullptr;
    }
#endif
    portal_session_handle_.clear();
    return false;
  }
  Log(LogLevel::Info, kRecorderContext, "Built GStreamer screen pipeline.");

  GstElement *splitmux_sink =
      gst_bin_get_by_name(GST_BIN(pipeline), "clipdeck_splitmux");
  if (splitmux_sink == nullptr) {
    gst_object_unref(pipeline);
    SetMessage("GStreamer pipeline does not expose splitmuxsink.", false);
    Log(LogLevel::Error, kRecorderContext, message_);
    if (portal_fd_ >= 0) {
      close(portal_fd_);
      portal_fd_ = -1;
      portal_node_id_ = 0;
      capture_source_type_.clear();
      portal_target_object_.reset();
    }
#if defined(CLIPDECK_HAS_GSTREAMER)
    ClosePortalSession(static_cast<GDBusConnection *>(portal_connection_),
                       portal_session_handle_);
    if (portal_connection_ != nullptr) {
      g_object_unref(static_cast<GDBusConnection *>(portal_connection_));
      portal_connection_ = nullptr;
    }
#endif
    portal_session_handle_.clear();
    return false;
  }

  GstBus *bus = gst_element_get_bus(pipeline);
  Log(LogLevel::Info, kRecorderContext,
      "Setting GStreamer screen pipeline to PLAYING.");
  const GstStateChangeReturn state_result =
      gst_element_set_state(pipeline, GST_STATE_PLAYING);
  Log(LogLevel::Info, kRecorderContext,
      "GStreamer screen pipeline PLAYING request returned " +
          StateChangeReturnName(state_result) + ".");

  if (state_result == GST_STATE_CHANGE_FAILURE) {
    const std::string message = PopStartupError(bus);
    gst_object_unref(bus);
    gst_object_unref(splitmux_sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    SetMessage(message, false);
    Log(LogLevel::Error, kRecorderContext, message_);
    if (portal_fd_ >= 0) {
      close(portal_fd_);
      portal_fd_ = -1;
      portal_node_id_ = 0;
      capture_source_type_.clear();
      portal_target_object_.reset();
    }
#if defined(CLIPDECK_HAS_GSTREAMER)
    ClosePortalSession(static_cast<GDBusConnection *>(portal_connection_),
                       portal_session_handle_);
    if (portal_connection_ != nullptr) {
      g_object_unref(static_cast<GDBusConnection *>(portal_connection_));
      portal_connection_ = nullptr;
    }
#endif
    portal_session_handle_.clear();
    return false;
  }

  pipeline_ = pipeline;
  splitmux_sink_ = splitmux_sink;
  bus_ = bus;
  running_ = true;
  SetMessage("recording", true);
  bus_thread_ =
      std::jthread([this](std::stop_token stop_token) { MonitorBus(stop_token); });

  Log(LogLevel::Info, kRecorderContext,
      std::format("Started native recorder at {}x{}@{}fps, {} kbps video.",
                  config_.width, config_.height, config_.fps,
                  config_.video_bitrate_kbps));
  return true;
#endif
}

void GStreamerRecorder::Stop() {
  if (!running_ && pipeline_ == nullptr && portal_fd_ < 0 &&
      portal_session_handle_.empty()) {
    return;
  }

  running_ = false;

  if (bus_thread_.joinable()) {
    bus_thread_.request_stop();
    bus_thread_.join();
  }

#if defined(CLIPDECK_HAS_GSTREAMER)
  if (pipeline_ != nullptr) {
    auto *pipeline = static_cast<GstElement *>(pipeline_);
    gst_element_set_state(pipeline, GST_STATE_NULL);
  }

  if (bus_ != nullptr) {
    gst_object_unref(static_cast<GstBus *>(bus_));
  }

  if (splitmux_sink_ != nullptr) {
    gst_object_unref(static_cast<GstElement *>(splitmux_sink_));
  }

  if (pipeline_ != nullptr) {
    gst_object_unref(static_cast<GstElement *>(pipeline_));
  }
#endif

  bus_ = nullptr;
  splitmux_sink_ = nullptr;
  pipeline_ = nullptr;
  if (portal_fd_ >= 0) {
    close(portal_fd_);
    portal_fd_ = -1;
    portal_node_id_ = 0;
    capture_source_type_.clear();
    portal_target_object_.reset();
  }
#if defined(CLIPDECK_HAS_GSTREAMER)
  ClosePortalSession(static_cast<GDBusConnection *>(portal_connection_),
                     portal_session_handle_);
  if (portal_connection_ != nullptr) {
    g_object_unref(static_cast<GDBusConnection *>(portal_connection_));
    portal_connection_ = nullptr;
  }
#endif
  portal_session_handle_.clear();
  SetMessage("stopped", false);
  Log(LogLevel::Info, kRecorderContext, "Stopped native recorder.");
}

bool GStreamerRecorder::SaveClip() {
  std::scoped_lock save_lock(save_mutex_);

  {
    std::scoped_lock lock(state_mutex_);
    last_save_failure_.clear();
  }

  std::string unhealthy_message;
  {
    std::scoped_lock lock(state_mutex_);
    if (!healthy_) {
      unhealthy_message = message_;
    }
  }

  if (!unhealthy_message.empty()) {
    Log(LogLevel::Error, kRecorderContext,
        "Native recorder is not healthy: " + unhealthy_message);
    {
      std::scoped_lock lock(state_mutex_);
      last_save_failure_ = "Native recorder is not healthy: " + unhealthy_message;
    }
    return false;
  }

  SplitCurrentSegment();
  if (!WaitForAnyFinalizedSegment(std::chrono::seconds(2))) {
    Log(LogLevel::Warning, kRecorderContext,
        "Recorder has not produced a finalized segment yet.");
    {
      std::scoped_lock lock(state_mutex_);
      last_save_failure_ =
          "Recorder has not produced a finalized segment yet.";
    }
    return false;
  }

  const auto segments = SelectSegmentsForClip();
  if (segments.empty()) {
    Log(LogLevel::Warning, kRecorderContext,
        "Recorder has no usable finalized segments for saving.");
    {
      std::scoped_lock lock(state_mutex_);
      last_save_failure_ =
          "Recorder has no usable finalized segments for saving.";
    }
    return false;
  }

  const double selected_duration_seconds =
      static_cast<double>(segments.size() * kSegmentSeconds);
  const auto clip_path = muxer_.WriteClipFromSegments(
      segments, ClipMuxerOptions{.target_duration =
                                     std::chrono::seconds(
                                         config_.clip_length_seconds),
                                 .width = config_.width,
                                 .height = config_.height,
                                 .fps = config_.fps,
                                 .video_bitrate_kbps =
                                     config_.video_bitrate_kbps,
                                 .audio_bitrate_kbps =
                                     config_.audio_bitrate_kbps,
                                 .audio_enabled = config_.audio_enabled,
                                 .trust_recorder_segments = true,
                                 .available_duration_seconds =
                                     selected_duration_seconds,
                                 .validate_black_frames = false});

  if (!clip_path.has_value()) {
    const std::string muxer_failure = muxer_.LastFailure();
    const std::string save_failure =
        muxer_failure.empty() ? "Native clip save failed." : muxer_failure;
    Log(LogLevel::Error, kRecorderContext,
        "Native clip save failed: " + save_failure);
    {
      std::scoped_lock lock(state_mutex_);
      last_save_failure_ = save_failure;
      if (!muxer_failure.empty()) {
        last_capture_anomaly_ = muxer_failure;
      }
    }
    return false;
  }

  Log(LogLevel::Info, kRecorderContext,
      "Stored clip: " + clip_path.value().string() + ".");
  {
    std::scoped_lock lock(state_mutex_);
    last_capture_anomaly_.clear();
    last_save_failure_.clear();
    last_saved_clip_ = clip_path.value();
  }
  return true;
}

RecorderStatus GStreamerRecorder::Status() const {
  bool running = false;
  bool healthy = false;
  std::string message;
  std::string capture_source_type;
  std::string last_capture_anomaly;
  std::string last_save_failure;
  std::filesystem::path last_saved_clip;
  {
    std::scoped_lock lock(state_mutex_);
    running = running_;
    healthy = healthy_;
    message = message_;
    capture_source_type = capture_source_type_;
    last_capture_anomaly = last_capture_anomaly_;
    last_save_failure = last_save_failure_;
    last_saved_clip = last_saved_clip_;
  }

  const auto count = SegmentCount();
  const auto segment_summary =
      SegmentSummary(count, config_.audio_enabled);
  return RecorderStatus{.running = running,
                        .healthy = healthy,
                        .backend = "native",
                        .message = message,
                        .capture_source_type = capture_source_type,
                        .buffered_duration =
                            std::chrono::seconds(count * kSegmentSeconds),
                        .buffered_bytes = SegmentBytes(),
                        .memory_budget_bytes = EstimateRecorderMemoryBudgetBytes(
                            config_),
                        .finalized_segment_count = segment_summary.count,
                        .finalized_segment_duration = segment_summary.duration,
                        .can_save_any_clip = segment_summary.count > 0,
                        .can_save_full_clip_without_padding =
                            segment_summary.duration >=
                            std::chrono::seconds(config_.clip_length_seconds),
                        .last_capture_anomaly = last_capture_anomaly,
                        .last_save_failure = last_save_failure,
                        .last_saved_clip = last_saved_clip,
                        .audio_sample_rate = segment_summary.audio_sample_rate,
                        .audio_channels = segment_summary.audio_channels,
                        .audio_enabled = config_.audio_enabled};
}

#if defined(CLIPDECK_ENABLE_RECORDER_TEST_HOOKS)
void GStreamerRecorder::SetPortalTestTargetObject(int fd, std::uint32_t node_id,
                                                  std::string target_object) {
  portal_fd_ = fd;
  portal_node_id_ = node_id;
  portal_target_object_ = std::move(target_object);
}

void GStreamerRecorder::SetPortalTestPath(int fd, std::uint32_t node_id) {
  portal_fd_ = fd;
  portal_node_id_ = node_id;
  portal_target_object_.reset();
}

void GStreamerRecorder::SetPortalTestCaptureSourceType(
    std::string source_type) {
  std::scoped_lock lock(state_mutex_);
  capture_source_type_ = std::move(source_type);
}

std::string GStreamerRecorder::BuildPipelineDescriptionForTest() const {
  return BuildPipelineDescription();
}
#endif

void GStreamerRecorder::MonitorBus(std::stop_token stop_token) {
#if defined(CLIPDECK_HAS_GSTREAMER)
  auto *bus = static_cast<GstBus *>(bus_);

  while (running_ && !stop_token.stop_requested()) {
    GstMessage *message =
        gst_bus_timed_pop_filtered(bus, 250 * GST_MSECOND,
                                   static_cast<GstMessageType>(GST_MESSAGE_ERROR |
                                                               GST_MESSAGE_EOS |
                                                               GST_MESSAGE_WARNING));

    if (message == nullptr) {
      continue;
    }

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError *error = nullptr;
      gchar *debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      const std::string text =
          error == nullptr ? "unknown GStreamer error" : error->message;
      const std::string debug_text = debug == nullptr ? "" : debug;
      SetMessage(debug_text.empty() ? text : text + " (" + debug_text + ")",
                 false);
      Log(LogLevel::Error, kRecorderContext,
          "GStreamer recorder error: " + text);
      if (debug != nullptr) {
        g_free(debug);
      }
      if (error != nullptr) {
        g_error_free(error);
      }
      running_ = false;
    }

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
      GError *error = nullptr;
      gchar *debug = nullptr;
      gst_message_parse_warning(message, &error, &debug);
      const std::string text =
          error == nullptr ? "unknown GStreamer warning" : error->message;
      Log(LogLevel::Warning, kRecorderContext,
          "GStreamer recorder warning: " + text);
      if (debug != nullptr) {
        g_free(debug);
      }
      if (error != nullptr) {
        g_error_free(error);
      }
    }

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
      SetMessage("GStreamer pipeline reached end of stream.", false);
      running_ = false;
    }

    gst_message_unref(message);
  }
#else
  (void)stop_token;
#endif
}

void GStreamerRecorder::CleanSegmentDirectory() const {
  std::error_code error;
  std::filesystem::remove_all(segment_directory_, error);
  error.clear();
  std::filesystem::create_directories(segment_directory_, error);

  if (error) {
    Log(LogLevel::Error, kRecorderContext,
        "Failed to create recorder segment directory: " + error.message());
  }
}

void GStreamerRecorder::SplitCurrentSegment() const {
#if defined(CLIPDECK_HAS_GSTREAMER)
  if (splitmux_sink_ == nullptr) {
    return;
  }

  g_signal_emit_by_name(static_cast<GstElement *>(splitmux_sink_), "split-now",
                        nullptr);
#endif
}

bool GStreamerRecorder::WaitForAnyFinalizedSegment(
    std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!SelectSegmentsForClip().empty()) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(75));
  }

  return !SelectSegmentsForClip().empty();
}

std::vector<std::filesystem::path> GStreamerRecorder::SelectSegmentsForClip() const {
  const auto selected_count = static_cast<std::size_t>(
      std::max(config_.clip_length_seconds +
                   std::clamp(config_.buffer_safety_seconds, 1, 3),
               1));
  return SelectRecentFinalizedSegmentsByCount(segment_directory_,
                                             selected_count);
}

std::size_t GStreamerRecorder::SegmentBytes() const {
  std::size_t bytes = 0;
  std::error_code error;

  if (!std::filesystem::exists(segment_directory_, error)) {
    return 0;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(segment_directory_, error)) {
    if (error || !entry.is_regular_file()) {
      continue;
    }

    std::error_code size_error;
    bytes += static_cast<std::size_t>(entry.file_size(size_error));
  }

  return bytes;
}

std::size_t GStreamerRecorder::SegmentCount() const {
  std::size_t count = 0;
  std::error_code error;

  if (!std::filesystem::exists(segment_directory_, error)) {
    return 0;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(segment_directory_, error)) {
    if (error || !entry.is_regular_file() || entry.path().extension() != ".mp4") {
      continue;
    }

    std::error_code size_error;
    if (entry.file_size(size_error) > 0 && !size_error) {
      ++count;
    }
  }

  return count;
}

std::string GStreamerRecorder::BuildPipelineDescription() const {
#if defined(CLIPDECK_HAS_GSTREAMER)
  EnsureGStreamerInitialized();
#endif
  std::string video_source =
      "pipewiresrc name=clipdeck_screen_source client-name=ClipDeck fd=" +
      std::to_string(portal_fd_);
  if (portal_target_object_.has_value()) {
    video_source += " target-object=" + GstQuote(portal_target_object_.value());
  } else {
    video_source += " path=" + GstQuote(std::to_string(portal_node_id_));
  }
  video_source += " do-timestamp=true";

  const std::string video_convert =
      std::format("videoconvert ! videoscale ! videorate ! "
                  "video/x-raw,format=I420,width={},height={},framerate={}/1",
                  config_.width, config_.height, config_.fps);

  const std::string video_encoder =
      config_.encoder == "x264"
          ? std::format("x264enc tune=zerolatency speed-preset=veryfast bitrate={} "
                        "key-int-max={} ! h264parse config-interval=-1 "
                        "! video/x-h264,stream-format=avc,alignment=au",
                        config_.video_bitrate_kbps, config_.fps)
          : std::format("openh264enc bitrate={} rate-control=bitrate "
                        "usage-type=screen gop-size={} ! h264parse "
                        "config-interval=-1 ! "
                        "video/x-h264,stream-format=avc,alignment=au",
                        config_.video_bitrate_kbps * 1000, config_.fps);

  const auto segment_pattern =
      (segment_directory_ / "clipdeck-%05d.mp4").string();
  const auto max_size_time =
      static_cast<std::uint64_t>(kSegmentSeconds) * 1000000000ULL;

  std::ostringstream pipeline;
  pipeline << "splitmuxsink name=clipdeck_splitmux "
           << "location=" << GstQuote(segment_pattern) << " "
           << "max-size-time=" << max_size_time << " "
           << "max-files=" << RetainedSegmentCount(config_) << " "
           << "async-finalize=true muxer-factory=mp4mux "
           << video_source
           << " ! " << LiveRawVideoQueue(config_) << " ! "
           << video_convert << " "
           << "! " << LiveRawVideoQueue(config_) << " ! "
           << video_encoder
           << " ! " << LiveEncodedVideoQueue(config_)
           << " ! clipdeck_splitmux.video ";

  if (config_.audio_enabled && !config_.audio_source.empty()) {
    const bool use_fdk_aac =
#if defined(CLIPDECK_HAS_GSTREAMER)
        GstElementAvailable("fdkaacenc");
#else
        false;
#endif
    const std::string audio_caps =
        use_fdk_aac ? "audio/x-raw,format=S16LE,rate=48000,channels=2"
                    : "audio/x-raw,format=F32LE,rate=48000,channels=2";
    const std::string audio_encoder =
        use_fdk_aac
            ? std::format("fdkaacenc bitrate={} ! aacparse",
                          config_.audio_bitrate_kbps * 1000)
            : std::format("avenc_aac bitrate={} ! aacparse",
                          config_.audio_bitrate_kbps * 1000);

    pipeline << "pulsesrc name=clipdeck_output_audio_source device="
             << GstQuote(config_.audio_source)
             << " do-timestamp=true "
             << "! " << LiveRawAudioQueue() << " "
             << "! audioconvert ! audioresample "
             << "! " << audio_caps << " "
             << "! " << audio_encoder
             << " ! " << LiveEncodedAudioQueue(config_)
             << " ! clipdeck_splitmux.audio_0 ";
  }

  return pipeline.str();
}

void GStreamerRecorder::SetMessage(std::string message, bool healthy) {
  std::scoped_lock lock(state_mutex_);
  message_ = std::move(message);
  healthy_ = healthy;
}

} // namespace clipdeck
