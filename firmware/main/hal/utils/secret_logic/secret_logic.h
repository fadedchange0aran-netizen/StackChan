/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <string>
#include <string_view>

namespace secret_logic {

std::string get_server_url();
std::string generate_auth_token();
std::string generate_handshake_token(std::string_view data);
void update_custom_config(const std::string& url, const std::string& key, const std::string& model, bool use_custom);

}  // namespace secret_logic
