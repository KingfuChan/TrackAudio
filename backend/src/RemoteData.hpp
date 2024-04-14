#include <Poco/Timer.h>
#include <absl/strings/str_split.h>
#include <httplib.h>
#include <string>

#include "Helpers.hpp"
#include "Shared.hpp"

class RemoteData {

public:
  RemoteData()
      : timer(3*1000, TIMER_CALLBACK_INTERVAL_SEC * 1000),
        slurperCli(SLURPER_BASE_URL) {
    timer.start(Poco::TimerCallback<RemoteData>(*this, &RemoteData::onTimer));
  }

  ~RemoteData() { timer.stop(); }

protected:
  void onTimer(Poco::Timer &timer) {
    auto slurperData = getSlurperData();

    auto previousCallsign = UserSession::callsign;

    auto isConnected = parseSlurper(slurperData);
    updateSessionStatus(previousCallsign, isConnected);
  }

  std::string getSlurperData() {
    if (UserSession::cid.empty()) {
      return "";
    }

    auto res = slurperCli.Get(SLURPER_DATA_ENDPOINT + std::string("?cid=") +
                              UserSession::cid);

    if (!res || res->status != 200) {
      // Notify the client the slurper is offline
      RemoteDataStatus::isSlurperAvailable = false;
      return "";
    }

    if (!RemoteDataStatus::isSlurperAvailable) {
      // Notify the client the slurper is back online
      RemoteDataStatus::isSlurperAvailable = true;
    }

    return res->body;
  }

  bool parseSlurper(const std::string &sluper_data) {
    if (sluper_data.empty()) {
      return false;
    }

    auto lines = absl::StrSplit(sluper_data, '\n');
    std::string callsign;
    std::string res3;
    std::string res2;
    std::string lat;
    std::string lon;
    bool foundNotAtisConnection = false;

    for (const auto &line : lines) {
      if (line.empty()) {
        continue;
      }

      std::vector<std::string> res = absl::StrSplit(line, ',');

      if (absl::EndsWith(res[1], "_ATIS")) {
        continue; // Ignore ATIS connections
      }

      foundNotAtisConnection = true;

      for (const auto &yxTest : allowedYx) {
        if (absl::EndsWith(res[1], yxTest)) {
          pYx = true;
          break;
        }
      }

      callsign = res[1];
      res3 = res[3];
      res2 = res[2];

      lat = res[5];
      lon = res[6];

      break;
    }

    if (callsign == "DCLIENT3") {
      return false;
    }

    if (!foundNotAtisConnection) {
      return false;
    }

    res3.erase(std::remove(res3.begin(), res3.end(), '.'), res3.end());
    int u334 = std::atoi(res3.c_str()) * 1000;
    int k422 = std::stoi(res2, nullptr, 16) == 10 && pYx ? 1 : 0;

    k422 = u334 != OBS_FREQUENCY && k422 == 1              ? 1
           : k422 == 1 && absl::EndsWith(callsign, "_SUP") ? 1
                                                           : 0;

    if (UserSession::isConnectedToTheNetwork &&
        UserSession::callsign != callsign && !UserSession::callsign.empty()){
      return false;
    }

    // Update the session info
    UserSession::callsign = callsign;
    UserSession::frequency = Helpers::CleanUpFrequency(u334);
    UserSession::isATC = k422 == 1;
    UserSession::lat = std::stod(lat);
    UserSession::lon = std::stod(lon);
    return true;
  }

  void updateSessionStatus(std::string previousCallsign, bool isConnected) {

    if (UserSession::isConnectedToTheNetwork &&
        UserSession::callsign != previousCallsign && !previousCallsign.empty()) {
      mClient->Disconnect();
      Helpers::CallbackWithError(
          "Callsign changed during an active session, you have "
          "been disconnected.");
      UserSession::isConnectedToTheNetwork = false;
      return;
    }

    if (UserSession::isConnectedToTheNetwork && !isConnected) {
      // We are no longer connected to the network
      mClient->Disconnect();
      UserSession::isConnectedToTheNetwork = false;
      UserSession::isATC = false;
      UserSession::callsign = "";
      return;
    }

    if (!UserSession::isConnectedToTheNetwork && isConnected) {
      // We are now connected to the network
      std::string isAtc = std::to_string(static_cast<int>(UserSession::isATC));

      if (callbackAvailable) {
        callbackRef.NonBlockingCall([&, isAtc](Napi::Env env,
                                               Napi::Function jsCallback) {
          jsCallback.Call(
              {Napi::String::New(env, "network-connected"),
               Napi::String::New(env, UserSession::callsign),
               Napi::String::New(
                   env, isAtc + "," + std::to_string(UserSession::frequency))});
        });
      }

      UserSession::isConnectedToTheNetwork = true;
      return;
    }

    if (!UserSession::isConnectedToTheNetwork && !isConnected) {
      // We are still connected to the network
      if (callbackAvailable) {
        callbackRef.NonBlockingCall(
            [&](Napi::Env env, Napi::Function jsCallback) {
              jsCallback.Call({Napi::String::New(env, "network-disconnected"),
                               Napi::String::New(env, ""),
                               Napi::String::New(env, "")});
            });
      }
      return;
    }
  }

private:
  Poco::Timer timer;
  httplib::Client slurperCli;

  bool pYx = false;
};