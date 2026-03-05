#pragma once

#define ssid "WIFI_NAME"
#define password "WIFI_PASS"
// #define USERNAME "admin"
// #define PASSWORD_HASH "03ac674216f3e15c761ee1a5e255f067953623c8b388b4459e13f978d7c846f4"

struct User {
  const char* username;
  const char* passwordHash;
};

const User USERS[] = {
  {"admin", "03ac674216f3e15c761ee1a5e255f067953623c8b388b4459e13f978d7c846f4"}, // 1234
  {"eu", "8d969eef6ecad3c29a3a629280e686cff8cae7a2f0b8c1227bcddc0e6d0e8f8c"}, // 123456
  {"tu", "e99a18c428cb38d5f260853678922e03abd8334d12f4d8f4e0e6c7e5d7e3e4b0"}   // abc123
};

const int USER_COUNT = sizeof(USERS) / sizeof(USERS[0]);