#include "service.h"
#include <cstdlib>
#include <QCoreApplication>
#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication qt(argc, argv);
    std::string database = "glass-music.sqlite3", assets = "assets", host = "127.0.0.1", promoteEmail;
    int port = 8787;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "glass-music-server [--db PATH] [--assets DIR] [--host IP] [--port PORT] [--promote-admin EMAIL]\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::runtime_error("Missing option value");
            const std::string value = argv[++i];
            if (option == "--db") database = value;
            else if (option == "--assets") assets = value;
            else if (option == "--promote-admin") promoteEmail = value;
            else if (option == "--host") host = value;
            else if (option == "--port") { size_t count = 0; port = std::stoi(value, &count); if (count != value.size() || port < 1 || port > 65535) throw std::runtime_error("Invalid port"); }
            else throw std::runtime_error("Unknown option: " + option);
        }
        if (!promoteEmail.empty()) {
            Database db(database);
            if (db.query("SELECT id FROM users WHERE lower(email)=lower(?)", {promoteEmail}).empty())
                throw std::runtime_error("Register this account first, then promote it.");
            db.execute("UPDATE users SET role='admin' WHERE lower(email)=lower(?)", {promoteEmail});
            std::cout << "Admin role granted. Sign in again to refresh the client.\n";
            return 0;
        }
        Service service(database, assets);
        drogon::app().setLogLevel(trantor::Logger::kWarn)
            .setClientMaxBodySize(32 * 1024 * 1024).setClientMaxMemoryBodySize(32 * 1024 * 1024).setThreadNum(4)
            // Prevent implicit static serving of the working directory/database.
            .setFileTypes({})
            .registerHandlerViaRegex("/api/v1/.*", [&service](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) { callback(service.handle(request)); },
                                     {drogon::Get,drogon::Head,drogon::Post,drogon::Put,drogon::Patch,drogon::Delete});
        std::cout << "Glass Music API listening on " << host << ':' << port << std::endl;
        drogon::app().addListener(host, uint16_t(port)).run();
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
