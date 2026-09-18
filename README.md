# Game Rooms C++ SDK

A small C++17 SDK for the Game Rooms protocol. It provides:

- HTTP endpoint helpers for creating rooms, fetching app configs, and looking up a room by code
- Built-in HTTP transport execution via libcurl so requests work out of the box
- `ecast`-style WebSocket envelope encoding/decoding
- Typed helpers for host/player object and relay operations
- Distinct room-not-found, room-locked, and room-full error classification

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Usage

```cpp
#include <game_rooms/sdk.hpp>
#include <iostream>

int main() {
  game_rooms::HttpApi api("https://example.com");
  auto create_response = api.create_room(game_rooms::Json::object({
    {"appId", "fibbage"},
    {"audienceEnabled", true}
  }));

  game_rooms::Session host(game_rooms::Role::host);
  auto message = host.create_object("state", game_rooms::Json::object({
    {"round", 1.0},
    {"prompt", "ready"}
  }));

  std::cout << create_response.status_code << "\n";
  std::cout << game_rooms::ProtocolCodec::encode(message) << "\n";
}
```

If you want a custom HTTP implementation, pass your own transport callback to `HttpApi` or set one later with `set_transport`.
