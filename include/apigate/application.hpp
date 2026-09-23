#ifndef APIGATE_APPLICATION_HPP_
#define APIGATE_APPLICATION_HPP_

#include <memory>

namespace apigate {

struct AppConfig;
class StructuredLogger;

class Application {
   public:
    Application(const AppConfig& config, StructuredLogger& logger);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    [[nodiscard]] int run();

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace apigate

#endif  // APIGATE_APPLICATION_HPP_
