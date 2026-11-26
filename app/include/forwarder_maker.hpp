//
//  forwarder_maker.hpp
//  Moonlight
//
//  Created by XITRIX on 23/01/2024.
//

#pragma once
#include <Settings.hpp>
#include <string>

int makeForwarder(const Host& host, const App& app, bool add_moonlight_logo = true);
int makeAppForwarder(const std::string& appName, const std::string& author);
void syncLevelUpLauncherOnStartup(const std::string& launchPath);
