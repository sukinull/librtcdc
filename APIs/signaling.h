#include <libwebsockets.h>
#include <stdio.h>
#include <uv.h>
#include <stdint.h>
#define DATASIZE 1024
#define NAMESIZE 32
typedef enum {
    FS_REGISTER_t = 0,
    FS_REGISTER_OK_t,
    FS_REPO_EXIST,
    FS_INIT_OK,
    FS_REPO_NOT_EXIST,
    FS_CONNECT_OK,
    FS_STATUS_OK,
    FS_UPLOAD_OK,
    FS_DOWNLOAD_OK,
    SY_INIT,
    SY_REPO_EXIST,
    SY_INIT_OK,
    SY_OAUTH,
    SY_OAUTH_OK,
    SY_OAUTH_FAIL,
    SY_CONNECT,
    SY_REPO_NOT_EXIST,
    SY_CONNECT_OK,
    SY_STATUS,
    SY_STATUS_OK,
    SY_UPLOAD,
    SY_UPLOAD_OK,
    SY_DOWNLOAD,
    SY_DOWNLOAD_OK,
    SY_SESSION_NOT_EXIST,

} METADATAtype;


typedef enum {
    FILESERVER_REGISTER_t = 5,
    FILESERVER_REGISTER_OK_t,
    FILESERVER_SDP_t,
    CLIENT_SDP_t,
} SIGNALDATAtype;


typedef enum {
    FILESERVER_INITIAL = 0,
    FILESERVER_READY,
    SIGNALSERVER_READY,
    SIGNALSERVER_RECV_REGISTER,
    CLIENT_INITIAL,
    CLIENT_WAIT,

    CLIENT_OFF,
} SESSIONstate;


struct URI_info_t{
    struct libwebsocket *fileserver_wsi;
    char repo_name[NAMESIZE];
    char URI_code[NAMESIZE];
};

struct session_info_t{
    struct libwebsocket *fileserver_wsi;
    struct libwebsocket *client_wsi;
    char repo_name[NAMESIZE];
    char session_id[NAMESIZE];
    METADATAtype state;
};

struct conn_info_t{
    struct libwebsocket *wsi;
    struct libwebsocket_context* context;
    volatile uint8_t *exit;
};


struct signal_session_data_t{
    SIGNALDATAtype type;
    char fileserver_dns[NAMESIZE];
    char client_dns[NAMESIZE];
    char SDP[DATASIZE];
    char candidates[DATASIZE];
};


struct conn_info_t* signal_initial(const char *address, int port, struct libwebsocket_protocols protocols[], char *protocol_name);

char *signal_getline(char **string);

void uv_signal_connect(uv_work_t *work);

void signal_connect(struct libwebsocket_context *context, volatile int *exit);

struct SDP_context* signal_req(struct conn_info_t* SDP_conn, char* peer_name);
