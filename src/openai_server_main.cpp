#include <Python.h>
#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

#include "acl_util.hpp"
#include "defs.hpp"
#include "qwen2_model.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace po = boost::program_options;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

static std::map<std::string, spdlog::level::level_enum> log_levels{
    {"trace", spdlog::level::trace}, {"debug", spdlog::level::debug},
    {"info", spdlog::level::info},   {"warning", spdlog::level::warn},
    {"error", spdlog::level::err},   {"critical", spdlog::level::critical},
    {"off", spdlog::level::off}};

struct ServerOptions {
  std::string host{"0.0.0.0"};
  uint16_t port{8000};
  std::string served_model_name{"qwen2"};
  bool use_chat_template{false};
};

struct GenerationResult {
  std::string text;
  int prompt_tokens{0};
  int completion_tokens{0};
  double elapsed_seconds{0.0};
  double tps{0.0};
  double ttft_seconds{0.0};
  double decode_seconds{0.0};
  double decode_tps{0.0};
};

class ClientDisconnected : public std::runtime_error {
public:
  explicit ClientDisconnected(const std::string &message)
      : std::runtime_error(message) {}
};

class CaptureStreamer : public BaseStreamer {
public:
  explicit CaptureStreamer(Qwen2HFTokenizer *tokenizer)
      : tokenizer_(tokenizer) {}

  auto put(const std::vector<int> &output_ids) -> void override {
    if (is_prompt_) {
      is_prompt_ = false;
      return;
    }

    static const std::vector<char> puncts{',', '!', ':', ';', '?'};

    token_cache_.insert(token_cache_.end(), output_ids.begin(),
                        output_ids.end());
    std::string text = tokenizer_->decode(token_cache_);
    if (text.empty()) {
      return;
    }

    std::string printable_text;
    if (text.back() == '\n') {
      printable_text = text.substr(print_len_);
      token_cache_.clear();
      print_len_ = 0;
    } else if (std::find(puncts.begin(), puncts.end(), text.back()) !=
               puncts.end()) {
    } else if (text.size() >= 3 && text.compare(text.size() - 3, 3, "�") == 0) {
    } else {
      printable_text = text.substr(print_len_);
      print_len_ = text.size();
    }

    text_ += printable_text;
  }

  auto end() -> void override {
    std::string text = tokenizer_->decode(token_cache_);
    text_ += text.substr(print_len_);
    is_prompt_ = true;
    token_cache_.clear();
    print_len_ = 0;
  }

  const std::string &text() const { return text_; }

private:
  Qwen2HFTokenizer *tokenizer_;
  bool is_prompt_{true};
  std::vector<int> token_cache_;
  int print_len_{0};
  std::string text_;
};

class SseStreamer : public BaseStreamer {
public:
  SseStreamer(tcp::socket &socket, Qwen2HFTokenizer *tokenizer,
              const std::string &model_name, bool chat)
      : socket_(socket), tokenizer_(tokenizer), model_name_(model_name),
        chat_(chat), id_((chat ? "chatcmpl-" : "cmpl-") +
                         std::to_string(std::time(nullptr))) {}

  auto put(const std::vector<int> &output_ids) -> void override {
    if (is_prompt_) {
      is_prompt_ = false;
      if (chat_) {
        send_chat_delta("", "assistant", nullptr);
      }
      return;
    }

    token_cache_.insert(token_cache_.end(), output_ids.begin(),
                        output_ids.end());
    std::string text = tokenizer_->decode(token_cache_);
    if (text.empty()) {
      return;
    }

    if (text.size() >= 3 && text.compare(text.size() - 3, 3, "�") == 0) {
      return;
    }

    std::string delta = text.substr(print_len_);
    if (delta.empty()) {
      return;
    }
    print_len_ = text.size();
    full_text_ += delta;

    if (chat_) {
      send_chat_delta(delta, "", nullptr);
    } else {
      send_completion_delta(delta, nullptr);
    }
  }

  auto end() -> void override {
    std::string text = tokenizer_->decode(token_cache_);
    std::string delta = text.substr(print_len_);
    if (!delta.empty()) {
      full_text_ += delta;
      if (chat_) {
        send_chat_delta(delta, "", nullptr);
      } else {
        send_completion_delta(delta, nullptr);
      }
    }
    token_cache_.clear();
    print_len_ = 0;
    is_prompt_ = true;
  }

  const std::string &full_text() const { return full_text_; }

  void send_finish() {
    if (chat_) {
      send_chat_delta("", "", "stop");
    } else {
      send_completion_delta("", "stop");
    }
    write_sse_data("[DONE]");
  }

private:
  void write_sse_data(const std::string &payload) {
    std::string frame = "data: " + payload + "\n\n";
    beast::error_code ec;
    asio::write(socket_, asio::buffer(frame), ec);
    if (ec) {
      disconnected_ = true;
      throw ClientDisconnected(ec.message());
    }
  }

  void send_chat_delta(const std::string &content, const std::string &role,
                       const char *finish_reason) {
    json delta = json::object();
    if (!role.empty()) {
      delta["role"] = role;
    }
    if (!content.empty()) {
      delta["content"] = content;
    }

    json chunk;
    chunk["id"] = id_;
    chunk["object"] = "chat.completion.chunk";
    chunk["created"] = std::time(nullptr);
    chunk["model"] = model_name_;
    chunk["choices"] = json::array(
        {{{"index", 0},
          {"delta", delta},
          {"finish_reason",
           finish_reason ? json(finish_reason) : json(nullptr)}}});
    write_sse_data(chunk.dump());
  }

  void send_completion_delta(const std::string &text,
                             const char *finish_reason) {
    json chunk;
    chunk["id"] = id_;
    chunk["object"] = "text_completion";
    chunk["created"] = std::time(nullptr);
    chunk["model"] = model_name_;
    chunk["choices"] = json::array(
        {{{"index", 0},
          {"text", text},
          {"finish_reason",
           finish_reason ? json(finish_reason) : json(nullptr)}}});
    write_sse_data(chunk.dump());
  }

  tcp::socket &socket_;
  Qwen2HFTokenizer *tokenizer_;
  std::string model_name_;
  bool chat_;
  std::string id_;
  bool is_prompt_{true};
  std::vector<int> token_cache_;
  int print_len_{0};
  std::string full_text_;
  bool disconnected_{false};
};

static std::string json_content_to_text(const json &content) {
  if (content.is_string()) {
    return content.get<std::string>();
  }
  if (!content.is_array()) {
    return "";
  }

  std::string text;
  for (const auto &part : content) {
    if (part.is_string()) {
      text += part.get<std::string>();
    } else if (part.is_object() && part.value("type", "") == "text" &&
               part.contains("text")) {
      text += part["text"].get<std::string>();
    }
  }
  return text;
}

static std::string build_qwen_chat_prompt(const json &messages,
                                          std::string *last_user_prompt) {
  std::string system_prompt = "You are a helpful assistant.";
  std::vector<std::pair<std::string, std::string>> turns;

  for (const auto &msg : messages) {
    std::string role = msg.value("role", "");
    std::string content = json_content_to_text(msg.value("content", json("")));
    if (role == "system") {
      system_prompt = content;
    } else if (role == "user" || role == "assistant") {
      turns.emplace_back(role, content);
      if (role == "user" && last_user_prompt) {
        *last_user_prompt = content;
      }
    }
  }

  std::ostringstream prompt;
  prompt << "<|im_start|>system\n" << system_prompt << "<|im_end|>";
  for (const auto &[role, content] : turns) {
    prompt << "\n<|im_start|>" << role << "\n"
           << content << "<|im_end|>";
  }
  prompt << "\n<|im_start|>assistant\n";
  return prompt.str();
}

static std::string get_last_user_prompt(const json &messages) {
  std::string prompt;
  for (const auto &msg : messages) {
    if (msg.value("role", "") == "user") {
      prompt = json_content_to_text(msg.value("content", json("")));
    }
  }
  return prompt;
}

static std::string decode_completion(Qwen2Model *model,
                                     const std::vector<int> &output_ids,
                                     size_t prompt_token_count) {
  std::vector<int> completion_ids(output_ids.begin() + prompt_token_count,
                                  output_ids.end());
  completion_ids.erase(
      std::remove_if(completion_ids.begin(), completion_ids.end(),
                     [model](int id) {
                       return model->qwen_tokenizer.is_special_id(id);
                     }),
      completion_ids.end());
  return model->qwen_tokenizer.decode(completion_ids);
}

static GenerationResult generate_text(Qwen2Model *model,
                                      const std::string &prompt,
                                      int max_tokens,
                                      float temperature,
                                      float top_p) {
  int requested_max_tokens = std::max(1, max_tokens);
  temperature = std::max(0.0f, temperature);
  top_p = std::min(1.0f, std::max(0.001f, top_p));

  int old_max_gen_len = model->config.max_gen_len;
  float old_temperature = model->config.temperature;
  float old_top_p = model->config.top_p;
  auto restore_model_config = [&]() {
    model->config.max_gen_len = old_max_gen_len;
    model->config.temperature = old_temperature;
    model->config.top_p = old_top_p;
    model->top_p_layer.SetParams(old_temperature, old_top_p);
  };

  model->config.temperature = temperature;
  model->config.top_p = top_p;
  model->top_p_layer.SetParams(temperature, top_p);

  InferenceCtx ctx(model, 0, 0);
  ctx.npu_stream = model->model_stream;
  std::vector<int> input_ids =
      model->qwen_tokenizer.encode(prompt, model->config.max_seq_len);

  if (static_cast<int>(input_ids.size()) >= model->config.max_seq_len) {
    int keep_prompt_tokens = std::max(1, model->config.max_seq_len - 1);
    input_ids.erase(input_ids.begin(), input_ids.end() - keep_prompt_tokens);
  }
  int available_gen_tokens =
      std::max(1, model->config.max_seq_len - static_cast<int>(input_ids.size()));
  int effective_max_tokens =
      std::min(requested_max_tokens, available_gen_tokens);
  model->config.max_gen_len = effective_max_tokens;

  if (effective_max_tokens != requested_max_tokens) {
    spdlog::warn("requested max_tokens {} exceeds remaining context {}; using "
                 "{} tokens instead",
                 requested_max_tokens, available_gen_tokens,
                 effective_max_tokens);
  }

  PerfStreamer perf;
  auto start = std::chrono::steady_clock::now();
  std::vector<int> output_ids;
  try {
    output_ids = model->Generate(input_ids, ctx, &perf);
  } catch (...) {
    restore_model_config();
    throw;
  }
  auto end = std::chrono::steady_clock::now();

  GenerationResult result;
  result.prompt_tokens = static_cast<int>(input_ids.size());
  result.completion_tokens =
      static_cast<int>(output_ids.size() - input_ids.size());
  result.elapsed_seconds =
      std::chrono::duration<double>(end - start).count();
  result.tps = result.elapsed_seconds > 0.0
                   ? result.completion_tokens / result.elapsed_seconds
                   : 0.0;
  result.ttft_seconds = perf.prompt_total_time_us() / 1000000.0;
  result.decode_seconds = perf.output_total_time_us() / 1000000.0;
  result.decode_tps = result.decode_seconds > 0.0
                          ? result.completion_tokens / result.decode_seconds
                          : 0.0;
  result.text = decode_completion(model, output_ids, input_ids.size());

  restore_model_config();
  return result;
}

static GenerationResult generate_text_stream(Qwen2Model *model,
                                             const std::string &prompt,
                                             int max_tokens,
                                             float temperature, float top_p,
                                             BaseStreamer *streamer) {
  int requested_max_tokens = std::max(1, max_tokens);
  temperature = std::max(0.0f, temperature);
  top_p = std::min(1.0f, std::max(0.001f, top_p));

  int old_max_gen_len = model->config.max_gen_len;
  float old_temperature = model->config.temperature;
  float old_top_p = model->config.top_p;
  auto restore_model_config = [&]() {
    model->config.max_gen_len = old_max_gen_len;
    model->config.temperature = old_temperature;
    model->config.top_p = old_top_p;
    model->top_p_layer.SetParams(old_temperature, old_top_p);
  };

  model->config.temperature = temperature;
  model->config.top_p = top_p;
  model->top_p_layer.SetParams(temperature, top_p);

  InferenceCtx ctx(model, 0, 0);
  ctx.npu_stream = model->model_stream;
  std::vector<int> input_ids =
      model->qwen_tokenizer.encode(prompt, model->config.max_seq_len);

  if (static_cast<int>(input_ids.size()) >= model->config.max_seq_len) {
    int keep_prompt_tokens = std::max(1, model->config.max_seq_len - 1);
    input_ids.erase(input_ids.begin(), input_ids.end() - keep_prompt_tokens);
  }
  int available_gen_tokens =
      std::max(1, model->config.max_seq_len - static_cast<int>(input_ids.size()));
  int effective_max_tokens =
      std::min(requested_max_tokens, available_gen_tokens);
  model->config.max_gen_len = effective_max_tokens;

  if (effective_max_tokens != requested_max_tokens) {
    spdlog::warn("requested max_tokens {} exceeds remaining context {}; using "
                 "{} tokens instead",
                 requested_max_tokens, available_gen_tokens,
                 effective_max_tokens);
  }

  PerfStreamer perf;
  StreamerGroup streamers({std::shared_ptr<BaseStreamer>(&perf,
                                                         [](BaseStreamer *) {}),
                           std::shared_ptr<BaseStreamer>(streamer,
                                                         [](BaseStreamer *) {})});

  auto start = std::chrono::steady_clock::now();
  std::vector<int> output_ids;
  try {
    output_ids = model->Generate(input_ids, ctx, &streamers);
  } catch (...) {
    restore_model_config();
    throw;
  }
  auto end = std::chrono::steady_clock::now();

  GenerationResult result;
  result.prompt_tokens = static_cast<int>(input_ids.size());
  result.completion_tokens =
      static_cast<int>(output_ids.size() - input_ids.size());
  result.elapsed_seconds = std::chrono::duration<double>(end - start).count();
  result.tps = result.elapsed_seconds > 0.0
                   ? result.completion_tokens / result.elapsed_seconds
                   : 0.0;
  result.ttft_seconds = perf.prompt_total_time_us() / 1000000.0;
  result.decode_seconds = perf.output_total_time_us() / 1000000.0;
  result.decode_tps = result.decode_seconds > 0.0
                          ? result.completion_tokens / result.decode_seconds
                          : 0.0;

  restore_model_config();
  return result;
}

static json make_chat_response(const std::string &model_name,
                               const GenerationResult &gen) {
  json res;
  res["id"] = "chatcmpl-" + std::to_string(std::time(nullptr));
  res["object"] = "chat.completion";
  res["created"] = std::time(nullptr);
  res["model"] = model_name;
  res["choices"] = json::array(
      {{{"index", 0},
        {"message", {{"role", "assistant"}, {"content", gen.text}}},
        {"finish_reason", "stop"}}});
  res["usage"] = {{"prompt_tokens", gen.prompt_tokens},
                  {"completion_tokens", gen.completion_tokens},
                  {"total_tokens", gen.prompt_tokens + gen.completion_tokens}};
  return res;
}

static json make_completion_response(const std::string &model_name,
                                     const GenerationResult &gen) {
  json res;
  res["id"] = "cmpl-" + std::to_string(std::time(nullptr));
  res["object"] = "text_completion";
  res["created"] = std::time(nullptr);
  res["model"] = model_name;
  res["choices"] =
      json::array({{{"index", 0}, {"text", gen.text}, {"finish_reason", "stop"}}});
  res["usage"] = {{"prompt_tokens", gen.prompt_tokens},
                  {"completion_tokens", gen.completion_tokens},
                  {"total_tokens", gen.prompt_tokens + gen.completion_tokens}};
  return res;
}

static http::response<http::string_body>
json_response(http::status status, const json &body, unsigned version,
              bool keep_alive) {
  http::response<http::string_body> res{status, version};
  res.set(http::field::server, "qwen_openai_server");
  res.set(http::field::content_type, "application/json");
  res.keep_alive(keep_alive);
  res.body() = body.dump();
  res.prepare_payload();
  return res;
}

static http::response<http::string_body>
server_busy_response(unsigned version, bool keep_alive) {
  return json_response(
      http::status::service_unavailable,
      {{"error",
        {{"message", "server busy, please wait"},
         {"type", "server_busy"}}}},
      version, keep_alive);
}

static bool is_generation_request(
    const http::request<http::string_body> &req) {
  if (req.method() != http::verb::post) {
    return false;
  }
  std::string target(req.target());
  return target == "/v1/chat/completions" || target == "/v1/completions";
}

static http::response<http::string_body>
handle_request(Qwen2Model *model, const ServerOptions &opts,
               const http::request<http::string_body> &req) {
  try {
    std::string target(req.target());

    if (req.method() == http::verb::get && target == "/health") {
      return json_response(http::status::ok, {{"status", "ok"}}, req.version(),
                           req.keep_alive());
    }

    if (req.method() == http::verb::get && target == "/v1/models") {
      json body = {{"object", "list"},
                   {"data",
                    json::array({{{"id", opts.served_model_name},
                                  {"object", "model"},
                                  {"owned_by", "local"}}})}};
      return json_response(http::status::ok, body, req.version(),
                           req.keep_alive());
    }

    if (req.method() != http::verb::post) {
      return json_response(http::status::method_not_allowed,
                           {{"error", {{"message", "method not allowed"}}}},
                           req.version(), req.keep_alive());
    }

    json body = json::parse(req.body());
    int max_tokens = body.value("max_tokens", model->config.max_gen_len);
    float temperature = body.value("temperature", model->config.temperature);
    float top_p = body.value("top_p", model->config.top_p);

    if (target == "/v1/chat/completions") {
      if (!body.contains("messages") || !body["messages"].is_array()) {
        return json_response(
            http::status::bad_request,
            {{"error", {{"message", "messages must be an array"}}}},
            req.version(), req.keep_alive());
      }
      std::string user_prompt = get_last_user_prompt(body["messages"]);
      std::string prompt =
          opts.use_chat_template
              ? build_qwen_chat_prompt(body["messages"], &user_prompt)
              : user_prompt;
      spdlog::info("user prompt: {}", user_prompt);
      GenerationResult gen =
          generate_text(model, prompt, max_tokens, temperature, top_p);
      spdlog::info("model response: {}", gen.text);
      spdlog::info("inference speed: total_tps {:.2f}, decode_tps {:.2f}, "
                   "ttft {:.3f}s, decode {:.3f}s, output tokens: {}, elapsed: "
                   "{:.3f}s",
                   gen.tps, gen.decode_tps, gen.ttft_seconds,
                   gen.decode_seconds, gen.completion_tokens,
                   gen.elapsed_seconds);
      return json_response(http::status::ok,
                           make_chat_response(opts.served_model_name, gen),
                           req.version(), req.keep_alive());
    }

    if (target == "/v1/completions") {
      std::string prompt;
      if (body.contains("prompt")) {
        prompt = json_content_to_text(body["prompt"]);
      }
      spdlog::info("user prompt: {}", prompt);
      GenerationResult gen =
          generate_text(model, prompt, max_tokens, temperature, top_p);
      spdlog::info("model response: {}", gen.text);
      spdlog::info("inference speed: total_tps {:.2f}, decode_tps {:.2f}, "
                   "ttft {:.3f}s, decode {:.3f}s, output tokens: {}, elapsed: "
                   "{:.3f}s",
                   gen.tps, gen.decode_tps, gen.ttft_seconds,
                   gen.decode_seconds, gen.completion_tokens,
                   gen.elapsed_seconds);
      return json_response(http::status::ok,
                           make_completion_response(opts.served_model_name, gen),
                           req.version(), req.keep_alive());
    }

    return json_response(http::status::not_found,
                         {{"error", {{"message", "not found"}}}},
                         req.version(), req.keep_alive());
  } catch (const std::exception &e) {
    spdlog::error("request failed: {}", e.what());
    return json_response(http::status::internal_server_error,
                         {{"error", {{"message", e.what()}}}}, req.version(),
                         req.keep_alive());
  }
}

static void write_sse_header(tcp::socket &socket) {
  std::string header =
      "HTTP/1.1 200 OK\r\n"
      "Server: qwen_openai_server\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: close\r\n"
      "\r\n";
  beast::error_code ec;
  asio::write(socket, asio::buffer(header), ec);
  if (ec) {
    throw ClientDisconnected(ec.message());
  }
}

static bool is_streaming_request(
    const http::request<http::string_body> &req) {
  if (req.method() != http::verb::post) {
    return false;
  }
  std::string target(req.target());
  if (target != "/v1/chat/completions" && target != "/v1/completions") {
    return false;
  }
  try {
    json body = json::parse(req.body());
    return body.value("stream", false);
  } catch (...) {
    return false;
  }
}

static void handle_streaming_request(
    Qwen2Model *model, const ServerOptions &opts,
    const http::request<http::string_body> &req, tcp::socket &socket) {
  std::string target(req.target());
  json body = json::parse(req.body());
  int max_tokens = body.value("max_tokens", model->config.max_gen_len);
  float temperature = body.value("temperature", model->config.temperature);
  float top_p = body.value("top_p", model->config.top_p);

  bool is_chat = target == "/v1/chat/completions";
  std::string user_prompt;
  std::string prompt;

  if (is_chat) {
    if (!body.contains("messages") || !body["messages"].is_array()) {
      json error = {{"error", {{"message", "messages must be an array"}}}};
      auto res = json_response(http::status::bad_request, error, req.version(),
                               req.keep_alive());
      http::write(socket, res);
      return;
    }
    user_prompt = get_last_user_prompt(body["messages"]);
    prompt = opts.use_chat_template
                 ? build_qwen_chat_prompt(body["messages"], &user_prompt)
                 : user_prompt;
  } else {
    if (body.contains("prompt")) {
      prompt = json_content_to_text(body["prompt"]);
    }
    user_prompt = prompt;
  }

  spdlog::info("user prompt: {}", user_prompt);
  write_sse_header(socket);

  SseStreamer sse(socket, &model->qwen_tokenizer, opts.served_model_name,
                  is_chat);
  try {
    GenerationResult gen = generate_text_stream(
        model, prompt, max_tokens, temperature, top_p, &sse);
    sse.send_finish();
    gen.text = sse.full_text();

    spdlog::info("model response: {}", gen.text);
    spdlog::info("inference speed: total_tps {:.2f}, decode_tps {:.2f}, "
                 "ttft {:.3f}s, decode {:.3f}s, output tokens: {}, elapsed: "
                 "{:.3f}s",
                 gen.tps, gen.decode_tps, gen.ttft_seconds,
                 gen.decode_seconds, gen.completion_tokens,
                 gen.elapsed_seconds);
  } catch (const ClientDisconnected &e) {
    spdlog::warn("stream client disconnected: {}", e.what());
  }
}

static bool validate_path(const std::string &path, bool directory) {
  if (!boost::filesystem::exists(path)) {
    return false;
  }
  return directory ? boost::filesystem::is_directory(path)
                   : boost::filesystem::is_regular_file(path);
}

static void handle_connection(tcp::socket socket, Qwen2Model *model,
                              const ServerOptions &server_opts,
                              std::atomic_bool *is_generating) {
  bool owns_generation = false;
  bool has_gil = false;
  PyGILState_STATE gil_state;

  try {
    beast::flat_buffer buffer;
    http::request<http::string_body> req;

    beast::error_code ec;
    http::read(socket, buffer, req, ec);
    if (ec) {
      spdlog::warn("failed to read request: {}", ec.message());
      return;
    }

    bool generation_request = is_generation_request(req);
    if (generation_request) {
      owns_generation = !is_generating->exchange(true);
      if (!owns_generation) {
        auto res = server_busy_response(req.version(), req.keep_alive());
        http::write(socket, res, ec);
        if (ec) {
          spdlog::warn("failed to write busy response: {}", ec.message());
        }
        socket.shutdown(tcp::socket::shutdown_send, ec);
        return;
      }
      gil_state = PyGILState_Ensure();
      has_gil = true;
    }

    try {
      if (is_streaming_request(req)) {
        handle_streaming_request(model, server_opts, req, socket);
      } else {
        auto res = handle_request(model, server_opts, req);
        http::write(socket, res, ec);
        if (ec) {
          spdlog::warn("failed to write response: {}", ec.message());
        }
      }
    } catch (const ClientDisconnected &e) {
      spdlog::warn("client disconnected: {}", e.what());
    } catch (const std::exception &e) {
      spdlog::error("connection handling failed: {}", e.what());
    }

    if (has_gil) {
      PyGILState_Release(gil_state);
      has_gil = false;
    }
    if (owns_generation) {
      is_generating->store(false);
      owns_generation = false;
    }
    socket.shutdown(tcp::socket::shutdown_send, ec);
  } catch (const std::exception &e) {
    spdlog::error("connection handling failed: {}", e.what());
    if (has_gil) {
      PyGILState_Release(gil_state);
    }
    if (owns_generation) {
      is_generating->store(false);
    }
  }
}

int main(int argc, char **argv) {
  Py_Initialize();
  PyImport_ImportModule("site");

  ModelConfig model_config;
  ServerOptions server_opts;
  std::string device_type;
  std::string log_level;
  std::string quant_method;

  try {
    po::options_description desc("qwen openai server options");
    desc.add_options()("help", "produce help message")(
        "host", po::value<std::string>(&server_opts.host)->default_value("0.0.0.0"),
        "http listen host")(
        "port", po::value<uint16_t>(&server_opts.port)->default_value(8000),
        "http listen port")(
        "served_model_name",
        po::value<std::string>(&server_opts.served_model_name)
            ->default_value("qwen2"),
        "model name returned in OpenAI responses")(
        "use_chat_template",
        po::value<bool>(&server_opts.use_chat_template)->default_value(false),
        "wrap chat messages with Qwen <|im_start|> template")(
        "max_seq_len",
        po::value<int>(&model_config.max_seq_len)->default_value(2048),
        "max sequence length")(
        "max_gen_token",
        po::value<int>(&model_config.max_gen_len)->default_value(512),
        "default max generated tokens")(
        "tokenizer", po::value<std::string>(&model_config.tok_path)->required(),
        "path to tokenizer directory")(
        "weight", po::value<std::string>(&model_config.model_path)->required(),
        "path to converted model weights")(
        "config", po::value<std::string>(&model_config.config_path)->required(),
        "path to model config.json")(
        "device_type", po::value<std::string>(&device_type)->default_value("npu"),
        "device type: npu")(
        "log_level", po::value<std::string>(&log_level)->default_value("info"),
        "log level")(
        "debug_print",
        po::value<bool>(&model_config.debug_print)->default_value(false),
        "print tensor values for debug")(
        "temperature",
        po::value<float>(&model_config.temperature)->default_value(0.6f),
        "default sampling temperature")(
        "top_p", po::value<float>(&model_config.top_p)->default_value(0.9f),
        "default top-p sampling value")(
        "rope_is_neox_style",
        po::value<bool>(&model_config.rope_is_neox_style)->default_value(true),
        "rope embedding style")(
        "quant_method", po::value<std::string>(&quant_method),
        "quant_method: awq_4bit");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help")) {
      std::cout << desc << "\n";
      Py_Finalize();
      return 0;
    }
    po::notify(vm);

    if (!log_levels.count(log_level)) {
      std::cerr << "invalid log_level: " << log_level << "\n";
      Py_Finalize();
      return 1;
    }
    spdlog::set_level(log_levels[log_level]);

    model_config.model_type = "qwen2";
    if (device_type != "npu") {
      spdlog::critical("qwen_openai_server currently supports only npu");
      Py_Finalize();
      return 1;
    }
    CHECK_ACL(aclInit(nullptr));
    model_config.device_type = DEV_NPU;

    if (!validate_path(model_config.tok_path, true) ||
        !validate_path(model_config.model_path, true) ||
        !validate_path(model_config.config_path, false)) {
      spdlog::critical("invalid model path, tokenizer path, or config path");
      Py_Finalize();
      return 1;
    }

    if (quant_method == "awq_4bit") {
      model_config.q_type = QuantType::AWQ_4B;
    }

    auto model = std::make_unique<Qwen2Model>();
    model->config = model_config;
    if (!model->Init()) {
      spdlog::critical("failed to init model");
      Py_Finalize();
      return 1;
    }

    asio::io_context ioc{1};
    tcp::endpoint endpoint{asio::ip::make_address(server_opts.host),
                           server_opts.port};
    tcp::acceptor acceptor{ioc, endpoint};
    spdlog::info("qwen_openai_server listening on {}:{}",
                 server_opts.host, server_opts.port);
    spdlog::info("model loaded, waiting for OpenAI HTTP requests");

    std::atomic_bool is_generating{false};
    PyEval_SaveThread();

    for (;;) {
      try {
        tcp::socket socket{ioc};
        acceptor.accept(socket);
        std::thread(handle_connection, std::move(socket), model.get(),
                    server_opts, &is_generating)
            .detach();
      } catch (const std::exception &e) {
        spdlog::error("accept failed: {}", e.what());
      }
    }
  } catch (const std::exception &e) {
    spdlog::critical("server failed: {}", e.what());
    Py_Finalize();
    return 1;
  }

  Py_Finalize();
  return 0;
}
