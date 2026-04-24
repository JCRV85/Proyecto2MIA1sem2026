#include "mia/engine.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

std::string LoadApiHost(const std::filesystem::path& workspace_root) {
  const auto config_path = workspace_root / "mia.config.json";
  std::ifstream config_file(config_path);
  if (!config_file) {
    return "0.0.0.0";
  }

  const auto config = nlohmann::json::parse(config_file, nullptr, false);
  if (config.is_discarded()) {
    return "0.0.0.0";
  }

  return config.value("apiHost", std::string{"0.0.0.0"});
}

}  // namespace

int main() {
  try {
    const auto workspace_root = std::filesystem::current_path();
    mia::Engine engine(workspace_root);
    const auto api_host = LoadApiHost(workspace_root);
    httplib::Server server;

    server.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "Content-Type"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
    });

    server.Options(R"(.*)", [](const httplib::Request&, httplib::Response& response) {
      response.status = 204;
    });

    server.Get("/api/state", [&](const httplib::Request&, httplib::Response& response) {
      nlohmann::json payload;
      payload["port"] = engine.api_port();
      payload["workspace"] = engine.workspace_root().string();
      payload["mounts"] = nlohmann::json::array();
      for (const auto& mount : engine.mounted_partitions()) {
        payload["mounts"].push_back({
            {"id", mount.id},
            {"path", mount.path},
            {"name", mount.name},
        });
      }

      const auto session = engine.session();
      payload["session"] = {
          {"active", session.active},
          {"user", session.user},
          {"group", session.group},
          {"partitionId", session.partition_id},
      };

      response.set_content(payload.dump(2), "application/json");
    });

    server.Post("/api/execute", [&](const httplib::Request& request, httplib::Response& response) {
      try {
        const auto payload = nlohmann::json::parse(request.body);
        const auto commands = payload.value("commands", std::string{});
        const auto result = engine.Execute(commands);
        nlohmann::json body = {
            {"output", result.output},
            {"artifacts", result.artifacts},
        };
        response.set_content(body.dump(2), "application/json");
      } catch (const std::exception& exception) {
        response.status = 400;
        response.set_content(
            nlohmann::json({{"error", exception.what()}}).dump(2),
            "application/json");
      }
    });

    const auto port = engine.api_port();
    std::cout << "API lista en http://" << api_host << ":" << port << std::endl;
    server.listen(api_host, port);
  } catch (const std::exception& exception) {
    std::cerr << "Error al iniciar la API: " << exception.what() << std::endl;
    return 1;
  }

  return 0;
}
