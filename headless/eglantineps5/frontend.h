#pragma once

#include <string>
#include "../multiplayer.h"

std::string SelectEglantinePS5Game(Eden::Multiplayer::RoomClient& rooms, const std::string& launch_error = {});
// Mounts the NFS shares of config/nfs.txt and waits for them (a game started by an /nfs/ path).
void MountNfsSharesNow();
