#include "msg.h"
#include "server.h"

void SV_InitSApi();
void SV_RunSApiFrame();
void SV_NotifySApiDisconnect(client_t* drop);
int SV_ConnectSApi(client_t* client);
void SV_SApiData(client_t* cl, msg_t* msg);
