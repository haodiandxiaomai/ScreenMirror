#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <cstdint>
#include <string>
#include "mirror_types.h"

int32_t ReadBE32(const uint8_t* p);
int64_t ReadBE64(const uint8_t* p);
FrameHeader ParseHeader(const uint8_t* buf);

// ====== 新的网络 API ======
// 启动 TCP 服务器，监听 0.0.0.0:port，返回监听 socket
SOCKET StartTcpServer(int port, std::wstring& error);
// 接受一个客户端连接，返回通信 socket
SOCKET AcceptTcpClient(SOCKET listenSock, std::wstring& error);

// 原有的工具函数保留
bool RecvAll(SOCKET s, uint8_t* dst, int size);
int SocketPendingBytes(SOCKET s);
int GetSocketOptInt(SOCKET s, int level, int optName);
void ConfigureVideoSocketForLowLatency(SOCKET s);