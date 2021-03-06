#include <stdio.h>
#include <syslog.h>
#include <signal.h>
#include <libwebsockets.h>
#include <string.h>
#include <jansson.h>
#include <bson.h>
#include <mongoc.h>
#include "ms_struct.h"
#include "../sy_lwst.h"

static struct libwebsocket_context *context;
static volatile int force_exit = 0;

static mongoc_client_t *client;

static mongoc_collection_t *fileserver_coll;
static mongoc_collection_t *session_coll;
static mongoc_collection_t *filesys_coll;

void delete_fileserver(struct libwebsocket *fileserver_wsi)
{
    bson_t *doc = bson_new();
    bson_error_t error;
    intptr_t wsi_ptr = (intptr_t)fileserver_wsi;
    BSON_APPEND_INT32 (doc, "wsi", wsi_ptr);

    if (!mongoc_collection_remove (fileserver_coll, MONGOC_DELETE_SINGLE_REMOVE, doc, NULL, &error)){
        printf ("%s\n", error.message);
    }
    bson_destroy(doc);
}


void delete_session(char *session_id)
{
    bson_t *doc = bson_new();
    bson_error_t error;
    BSON_APPEND_UTF8 (doc, "session_id", session_id);

    if (!mongoc_collection_remove (session_coll, MONGOC_DELETE_SINGLE_REMOVE, doc, NULL, &error)){
        printf ("%s\n", error.message);
    }
    bson_destroy(doc);
}

uint8_t insert_fileserver(char *fileserver_name, struct libwebsocket *fileserver_wsi)
{
    bson_t *doc = bson_new();
    bson_oid_t oid;
    bson_error_t error;

    bson_oid_init (&oid, NULL);
    BSON_APPEND_OID (doc, "_id", &oid);
    BSON_APPEND_UTF8 (doc, "fileserver_name", fileserver_name);
    intptr_t wsi_ptr = (intptr_t)fileserver_wsi;
    BSON_APPEND_INT32 (doc, "wsi", wsi_ptr);

    if (!mongoc_collection_insert (fileserver_coll, MONGOC_INSERT_NONE, doc, NULL, &error)) {
#ifdef DEBUG_META
        fprintf(stderr, "METADATA_SERVER: insert_fileserver(): %s\n", error.message);
#endif
        return 0;
    }
    bson_destroy(doc);
    return 1;
}


/* TODO: insert_session() needs to be revise because I add "files" field in mongoDB */
char *insert_session(struct libwebsocket *fileserver_wsi, struct libwebsocket *client_wsi, char *repo_name, METADATAtype state)
{
    char *session_id = (char *)calloc(1, 25);
    bson_oid_t oid;
    bson_oid_init (&oid, NULL);
    bson_oid_to_string(&oid, session_id);
    /*
       bson_t *doc = bson_new();*/


    intptr_t client_wsi_ptr = (intptr_t)client_wsi;
    intptr_t fileserver_wsi_ptr = (intptr_t)fileserver_wsi;

    bson_t insert_bson, file_array_bson, diff_array_bson;
    bson_error_t error;
    bson_init(&insert_bson);
    bson_init(&file_array_bson);
    bson_init(&diff_array_bson);
    bson_append_utf8(&insert_bson, "session_id", strlen("session_id"), session_id, strlen(session_id));
    bson_append_int32(&insert_bson, "client_wsi", strlen("client_wsi"), client_wsi_ptr);
    bson_append_int32(&insert_bson, "fileserver_wsi", strlen("fileserver_wsi"), fileserver_wsi_ptr);
    bson_append_utf8(&insert_bson, "repo_name", strlen("repo_name"), repo_name, strlen(repo_name));
    bson_append_int32(&insert_bson, "state", strlen("state"), state);
    bson_append_array(&insert_bson, "files", strlen("files"), (const bson_t *)&file_array_bson);
    bson_append_array(&insert_bson, "diff", strlen("diff"), (const bson_t *)&diff_array_bson);
    /*
       BSON_APPEND_UTF8 (doc, "session_id", session_id);
       BSON_APPEND_INT32 (doc, "client_wsi", client_wsi_ptr);
       BSON_APPEND_INT32 (doc, "fileserver_wsi", fileserver_wsi_ptr);
       BSON_APPEND_UTF8 (doc, "repo_name", repo_name);
       BSON_APPEND_INT32 (doc, "state", state);
       */
    if (!mongoc_collection_insert (session_coll, MONGOC_INSERT_NONE, &insert_bson, NULL, &error)){
#ifdef DEBUG_META
        fprintf (stderr, "METADATA_SERVER: insert_session(): %s\n", error.message);
#endif
        return NULL;
    }
    bson_destroy(&insert_bson);
    bson_destroy(&file_array_bson);
    return session_id;
}


uint8_t insert_filesys(struct ms_URI_info_t *repo_URI)
{
    bson_t *doc = bson_new();
    bson_error_t error;

    BSON_APPEND_UTF8 (doc, "URI_code", repo_URI->URI_code);
    intptr_t fileserver_wsi_ptr = (intptr_t)repo_URI->fileserver_wsi;
    BSON_APPEND_INT32 (doc, "fileserver_wsi", fileserver_wsi_ptr);
    BSON_APPEND_UTF8 (doc, "repo_name", repo_URI->repo_name);

    if (!mongoc_collection_insert (filesys_coll, MONGOC_INSERT_NONE, doc, NULL, &error)){
#ifdef DEBUG_META
        fprintf (stderr, "METADATA_SERVER: insert_filesys(): %s\n", error.message);
#endif
        return 0;
    }
    return 1;
}

/* TODO: this function needs to refactor*/
uint8_t update_session(char *session_id, char *field, void *value)
{
    bson_t *update = NULL;
    bson_t *query = NULL;
    bson_error_t error;
    query = BCON_NEW ("session_id", BCON_UTF8(session_id));

    if(strcmp(field, "client_wsi") == 0){
        intptr_t client_wsi_ptr = (intptr_t)value;
        update = BCON_NEW ("$set", "{",
                "client_wsi", BCON_INT32 (client_wsi_ptr),
                "}");
    }
    else if (strcmp(field, "state") == 0){
        update = BCON_NEW ("$set", "{",
                "state", BCON_INT32 (*(int *)value),
                "}");
    }
    else if (strcmp(field, "client_checksum") == 0 )
    {
        json_t *files_json = (json_t *)value;
        size_t index;
        json_t *file_json;
        char *filename, *client_checksum;

        bson_t update_bson, set_bson, file_array_bson, file_bson;
        bson_init(&update_bson);
        bson_init(&set_bson);
        bson_init(&file_array_bson);
        bson_init(&file_bson);
        bson_append_document_begin(&update_bson, "$set", 4, &set_bson);
        json_array_foreach(files_json, index, file_json){
            json_unpack(file_json, "{s:s, s:s}", "filename", &filename, "client_checksum", &client_checksum);
            bson_reinit(&file_bson);
            bson_append_utf8(&file_bson, "filename", 8, filename, strlen(filename));
            bson_append_utf8(&file_bson, "client_checksum", 15, client_checksum, strlen(client_checksum));
            bson_append_document(&file_array_bson, NULL, 0, (const bson_t *)&file_bson);
        }

        bson_append_array(&set_bson, "files", 5, (const bson_t*)&file_array_bson) ;
        bson_append_document_end(&update_bson, &set_bson);

        update = &update_bson;
    }
    else if (strcmp(field, "files") == 0)
    {
        json_t *file_array_json = (json_t *)value;
        size_t index;
        json_t *file_json;
        bson_t update_bson, set_bson, file_array_bson, file_bson;
        char *filename, *client_checksum, *fs_checksum;
        
        bson_init(&update_bson);
        bson_init(&set_bson);
        bson_init(&file_array_bson);
        bson_init(&file_bson);
        bson_append_document_begin(&update_bson, "$set", 4, &set_bson);
        json_array_foreach(file_array_json, index, file_json){
            json_unpack(file_json, "{s:s, s:s, s:s}", "filename", &filename, "client_checksum", &client_checksum, "fs_checksum", &fs_checksum);
            bson_reinit(&file_bson);
            bson_append_utf8(&file_bson, "filename", 8, filename, strlen(filename));
            bson_append_utf8(&file_bson, "client_checksum", 15, client_checksum, strlen(client_checksum));
            bson_append_utf8(&file_bson, "fs_checksum", 11, fs_checksum, strlen(fs_checksum));
            bson_append_document(&file_array_bson, NULL, 0, (const bson_t *)&file_bson);
        }

        bson_append_array(&set_bson, "files", 5, (const bson_t*)&file_array_bson) ;
        bson_append_document_end(&update_bson, &set_bson);

        update = &update_bson;


    }
    else if (strcmp(field, "diff") == 0)
    {
        json_t *diff_array_json = (json_t *)value;
        size_t index;
        json_t *diff_element_json;

        char *filename;
        int dirty;

        bson_t update_bson, set_bson, diff_array_bson, diff_element_bson;
        bson_init(&update_bson);
        bson_init(&set_bson);
        bson_init(&diff_array_bson);
        bson_init(&diff_element_bson);
        
        bson_append_document_begin(&update_bson, "$set", 4, &set_bson);
        json_array_foreach(diff_array_json, index, diff_element_json){
            json_unpack(diff_element_json, "{s:s, s:i}", "filename", &filename, "dirty", &dirty);
            bson_append_utf8(&diff_element_bson, "filename", 8, filename, strlen(filename));
            bson_append_int32(&diff_element_bson, "dirty", 5, dirty);
            bson_append_document(&diff_array_bson, NULL, 0, (const bson_t *)&diff_element_bson);
            bson_reinit(&diff_element_bson);
        }
        bson_append_array(&set_bson, "diff", 4, (const bson_t *)&diff_array_bson);
        bson_append_document_end(&update_bson, &set_bson);
        update = &update_bson;
    }
    else{
#ifdef DEBUG_META
        fprintf(stderr, "METADATA_SERVER: update_session(): the field not exist or the field cannot be changed(e.g repo_name)\n");
#endif
        return 0;
    }

    if (!mongoc_collection_update (session_coll, MONGOC_UPDATE_NONE, query, update, NULL, &error)) {
#if DEBUG_META
        fprintf (stderr, "METADATA_SERVER: update_session(): %s\n", error.message);
#endif
        bson_destroy(update);
        bson_destroy(query);
        return 0;
    }
    bson_destroy(update);
    bson_destroy(query);
    return 1;
}

struct ms_session_info_t *get_session(char *session_id)
{
    mongoc_cursor_t *cursor = NULL;
    const bson_t *find_context = NULL ;
    bson_t *find_query = bson_new();
    bson_t *proj = bson_new();
    BSON_APPEND_UTF8(find_query, "session_id", session_id);
    BSON_APPEND_BOOL(proj, "fileserver_wsi", true);
    BSON_APPEND_BOOL(proj, "client_wsi", true);
    BSON_APPEND_BOOL(proj, "repo_name", true);
    BSON_APPEND_BOOL(proj, "files", true);
    BSON_APPEND_BOOL(proj, "diff", true);
    /* TODO proj may be deleted */
    cursor = mongoc_collection_find (session_coll, MONGOC_QUERY_NONE, 0, 0, 0, find_query, proj, NULL);

    struct ms_session_info_t *session = NULL;
    if ( mongoc_cursor_next(cursor, &find_context) == 0){
#ifdef DEBUG_META
        fprintf(stderr, "METADATA_SERVER: session_id %s does not exist in db.session\n", session_id);
#endif
        return NULL;
    }
    else{
        char *find_context_str;
        find_context_str = bson_as_json(find_context, NULL);

        json_error_t *err = NULL;
        json_t *find_context_json = json_loads((const char *)find_context_str, JSON_DECODE_ANY, err);

        json_t *fileserver_wsi_json = json_object_get(find_context_json, "fileserver_wsi");
        json_t *client_wsi_json = json_object_get(find_context_json, "client_wsi");
        json_t *repo_name_json = json_object_get(find_context_json, "repo_name");
        json_t *files_json = json_object_get(find_context_json, "files");
        json_t *diff_json = json_object_get(find_context_json, "diff");
        
        session = (struct ms_session_info_t *)calloc(1, sizeof(struct ms_session_info_t));

        intptr_t fileserver_wsi_ptr = (intptr_t)((int)json_integer_value(fileserver_wsi_json));
        session->fileserver_wsi = (struct libwebsocket *)fileserver_wsi_ptr;
        intptr_t client_wsi_ptr = (intptr_t)json_integer_value(client_wsi_json);
        session->client_wsi = (struct libwebsocket *) client_wsi_ptr;
        strcpy(session->repo_name, json_string_value(repo_name_json));
        strcpy(session->files, json_dumps(files_json, 0));
        strcpy(session->diff, json_dumps(diff_json, 0));

        json_decref(files_json);
        json_decref(repo_name_json);
        json_decref(client_wsi_json);
        json_decref(fileserver_wsi_json);
        json_decref(diff_json);
        bson_free(find_context_str);

        strcpy(session->session_id, session_id);
    }
    return session;
}

struct ms_URI_info_t *get_URI(char *URI_code)
{
    mongoc_cursor_t *cursor = NULL;
    const bson_t *find_context = NULL ;
    bson_t *find_query = bson_new();
    bson_t *proj = bson_new();
    BSON_APPEND_UTF8(find_query, "URI_code", URI_code);
    BSON_APPEND_BOOL(proj, "fileserver_wsi", true);
    BSON_APPEND_BOOL(proj, "repo_name", true);
    cursor = mongoc_collection_find (filesys_coll, MONGOC_QUERY_NONE, 0, 0, 0, find_query, proj, NULL);

    struct ms_URI_info_t *URI = NULL;
    if ( mongoc_cursor_next(cursor, &find_context) == 0){
#ifdef DEBUG_META
        fprintf(stderr, "METADATA_SERVER: URI_code %s does not exist in db.filesys\n", URI_code);
#endif
        return NULL;
    }
    else{
        URI = (struct ms_URI_info_t *)calloc(1, sizeof(struct ms_URI_info_t));
        bson_iter_t iter, value;
        bson_iter_init(&iter, find_context);
        bson_iter_find_descendant(&iter, "fileserver_wsi", &value);
        BSON_ITER_HOLDS_INT32(&value);
        intptr_t fileserver_wsi_ptr = (intptr_t) bson_iter_int32(&value);
        URI->fileserver_wsi = (struct libwebsocket *)fileserver_wsi_ptr;

        bson_iter_init(&iter, find_context);
        bson_iter_find_descendant(&iter, "repo_name", &value);
        BSON_ITER_HOLDS_UTF8(&value);
        uint32_t length;
        strcpy(URI->repo_name, (char *)bson_iter_utf8(&value, &length));

        strcpy(URI->URI_code, URI_code);
    }
    return URI;
}

struct libwebsocket* get_wsi(char *fileserver_name)
{
    mongoc_cursor_t *cursor = NULL;
    const bson_t *find_context = NULL ;
    bson_t *find_query = bson_new();
    bson_t *proj = bson_new();
    BSON_APPEND_UTF8(find_query, "fileserver_name", fileserver_name);
    BSON_APPEND_BOOL(proj, "wsi", true);
    cursor = mongoc_collection_find (fileserver_coll, MONGOC_QUERY_NONE, 0, 0, 0, find_query, proj, NULL);

    if ( mongoc_cursor_next(cursor, &find_context) == 0){
#ifdef DEBUG_META
        fprintf(stderr, "METADATA_SERVER: fileserver name %s does not exist in db.fileserver\n", fileserver_name);
#endif
        return NULL;
    }
    else{
        bson_iter_t iter, value;
        bson_iter_init(&iter, find_context);
        bson_iter_find_descendant(&iter, "wsi", &value);
        BSON_ITER_HOLDS_INT32(&value);
        intptr_t wsi_ptr = (intptr_t) bson_iter_int32(&value);
        struct libwebsocket *wsi = (struct libwebsocket *)wsi_ptr;
        mongoc_cursor_destroy(cursor);
        /*TODO: I am not sure it is correct?*/
        bson_destroy(find_query);
        bson_destroy(proj);
        return wsi;
    }
}




/* TODO: it needs to check unfreed memory */
json_t *join_checksum_arrays(char *pivot_key, json_t *A_array_json, char *A_key, json_t *B_array_json, char *B_key)
{
    json_t *file_array_json = json_array();
    json_t *A_element_json, *B_element_json, *file_element_json; 
    size_t A_array_index, B_array_index, file_array_index, file_array_size;
    char *filename, *A_checksum, *B_checksum;

    // copy A_array_json to file_array_json
    json_array_foreach(A_array_json, A_array_index, A_element_json){
        file_element_json = json_copy(A_element_json);
        json_object_set(file_element_json, B_key, json_string(""));
        json_array_append(file_array_json, file_element_json);
    }

    file_array_size = json_array_size(file_array_json);
    // B_array_json join into file_array_json
    json_array_foreach(B_array_json, B_array_index, B_element_json){
        // extract the filename and B_checksum
        json_unpack(B_element_json, "{s:s, s:s}", pivot_key, &filename, B_key, &B_checksum);

        // check the filename is existed in file_array_json
        json_array_foreach(file_array_json, file_array_index, file_element_json){
            // if the filename is existed in file_array_json, insert B_checksum in it 
            if(strcmp(json_string_value(json_object_get(file_element_json, pivot_key)), filename) == 0)
            {
                json_object_set(file_element_json, B_key, json_string(B_checksum));
                break;
            }
        }

        // if the filename is not existed in file_array_json, append it
        if(file_array_index == file_array_size)
        {
            json_t *new_element_json = json_copy(B_element_json);
            json_object_set(new_element_json, A_key, json_string(""));
            json_array_append(file_array_json, new_element_json);
            file_array_size++;
        } 

    }

    return file_array_json;
}

static int callback_http(struct libwebsocket_context *context,
        struct libwebsocket *wsi,
        enum libwebsocket_callback_reasons reason, void *user,
        void *in, size_t len)
{
    switch (reason) {
        case LWS_CALLBACK_HTTP:
            break;
        default:
            break;
    }
    return 0;
}

static int callback_SDP(struct libwebsocket_context *context,
        struct libwebsocket *wsi,
        enum libwebsocket_callback_reasons reason, void *user,
        void *in, size_t len)
{
    struct lwst_writedata_t **write_data_ptr = (struct lwst_writedata_t **)user;
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            {
                fprintf(stderr, "==================================================================\n");
                fprintf(stderr, "METADATA_SERVER: LWS_CALLBACK_ESTABLISHED\n");
                /* initial write data */
                struct lwst_writedata_t *write_data = (struct lwst_writedata_t *)calloc(1, sizeof(struct lwst_writedata_t));
                *write_data_ptr = write_data;
                break;
            }
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            fprintf(stderr, "METADATA_SERVER: LWS_CALLBACK_CLIENT_CONNECTION_ERROR\n");
            break;
        case LWS_CALLBACK_CLOSED:
            fprintf(stderr, "METADATA_SERVER: LWS_CALLBACK_CLOSED\n");
            // deregister the fileserver name and fileserver wsi in mongoDB
            /* free write data */
            struct lwst_writedata_t *write_data = *write_data_ptr;
            free(write_data);
            /* TODO: delete_fileserver happenrd when fileserver closed, how about the client closed? */
            delete_fileserver(wsi);
            break;
        case LWS_CALLBACK_RECEIVE:
            {
                int metadata_type ,lws_err;
                char *recvd_data_str = (char *)in;
                json_t *recvd_session_JData = NULL;
                json_error_t json_err;
                recvd_session_JData = json_loads((const char *)recvd_data_str, JSON_DECODE_ANY, &json_err);
                json_unpack(recvd_session_JData, "{s:i}", "metadata_type", &metadata_type);
                switch (metadata_type){
                    case FS_REGISTER_t:
                        {
                            fprintf(stderr, "METADATA_SERVER: receive FS_REGISTER_t\n");
                            // insert the fileserver info in mongoDB
                            if(insert_fileserver("toppano://server1.tw", wsi) == 0){
                                /* TODO: if insert failed, what to do? */
                            }
                            //send FILESERVER_REGISTER_OK_t back to file server 
                            // the file server dns is assigned by signaling server
                            json_t *sent_session_JData = json_object();
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(FS_REGISTER_OK_t));
                            json_object_set_new(sent_session_JData, "fileserver_dns", json_string("toppano://server1.tw"));
                            char *sent_data_str = json_dumps(sent_session_JData, 0);

                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            write_data->target_wsi = wsi;
                            write_data->type = FS_REGISTER_OK_t;
                            strcpy(write_data->data, sent_data_str);

                            libwebsocket_callback_on_writable(context, wsi);

                            json_decref(recvd_session_JData);  
                            json_decref(sent_session_JData);
                            free(sent_data_str);
                            
                            break;
                        }
                    case SY_INIT:
                        {
                            fprintf(stderr, "METADATA_SERVER: receive SY_INIT\n");
                            /* Oauth */
                            /* Insert (client_wsi, fileserver_wsi, repo_name, state, session_id) into session table */
                            char *session_id, *repo_name;
                            /* TODO: exception when  "toppano://server1.tw" not exist */
                            struct libwebsocket *fileserver_wsi = get_wsi("toppano://server1.tw");
                            if(fileserver_wsi == NULL)
                            {
                                fprintf(stderr, "METADATA_SERVER: %s not exist\n", "toppano://server1.tw");
                                break;
                            }
                            struct libwebsocket *client_wsi = wsi;
                            json_unpack(recvd_session_JData, "{s:s}", "repo_name", &repo_name);
                            if((session_id = insert_session(fileserver_wsi, client_wsi, repo_name, SY_INIT)) == NULL){
                                /* TODO: if insert failed, what to do? */
                            }
                            /* send SY_INIT & repo_name & session_id to the fileserver*/
                            json_t *sent_session_JData = json_object();
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(SY_INIT));
                            json_object_set_new(sent_session_JData, "repo_name", json_string(repo_name));
                            json_object_set_new(sent_session_JData, "session_id", json_string(session_id));
                            char *sent_data_str = json_dumps(sent_session_JData, 0);

                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            write_data->target_wsi = fileserver_wsi;
                            write_data->type = SY_INIT;
                            strcpy(write_data->data, sent_data_str);

                            libwebsocket_callback_on_writable(context, wsi);

                            /* free memory */
                            json_decref(recvd_session_JData);   
                            json_decref(sent_session_JData);
                            free(sent_data_str);
                            free(session_id);
                            /*TODO: uniray7: I dont know why i cannot free repo_name which is used by json_unpack()
                             * if free, metadata server will segmentation fault
                             * */ 
                            //free(repo_name);
                            break;
                        }
                    case FS_INIT_OK:
                        {                    
                            fprintf(stderr, "METADATA_SERVER: receive FS_INIT_OK\n");
                            char *session_id = NULL; 
                            json_unpack(recvd_session_JData, "{s:s}", "session_id", &session_id);
                            /* Gen a URI_code and insert into Filesys tree (URI_code, repo_name, fileserver_wsi) */
                            char URI_code[ID_SIZE];
                            bson_oid_t oid;
                            bson_oid_init (&oid, NULL);
                            bson_oid_to_string(&oid, URI_code);
                            /* lookup fileserver_wsi and repo_name */
                            struct ms_session_info_t *session = get_session(session_id);
                            struct ms_URI_info_t *repo_URI = (struct ms_URI_info_t *)calloc(1, sizeof(struct ms_URI_info_t));
                            strcpy(repo_URI->URI_code, URI_code);
                            strcpy(repo_URI->repo_name, session->repo_name);
                            repo_URI->fileserver_wsi = session->fileserver_wsi;

                            if(insert_filesys(repo_URI) == 0){
                                /* TODO: if insert failed, what to do? */
                            }
                            /* update the session state */
                            METADATAtype state = SY_INIT_OK;
                            if(update_session(session_id, "state", (void*)&state) == 0){
                                /* TODO: if update failed, what to do? */
                            }
                            /* send SY_INIT_OK, URI_code, session_id to client according to client_wsi in session table */
                            json_t *sent_session_JData = json_object();
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(SY_INIT_OK));
                            json_object_set_new(sent_session_JData, "URI_code", json_string(URI_code));
                            json_object_set_new(sent_session_JData, "session_id", json_string(session_id));
                            char *sent_data_str = json_dumps(sent_session_JData, 0);

                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            write_data->target_wsi = session->client_wsi;
                            write_data->type = SY_INIT_OK;
                            strcpy(write_data->data, sent_data_str);

                            libwebsocket_callback_on_writable(context, wsi);

                            json_decref(recvd_session_JData);   
                            json_decref(sent_session_JData);
                            free(sent_data_str);
                            free(session);
                            free(repo_URI);
                            /*TODO: uniray7: I dont know why i cannot free session which is used by json_unpack()
                             * if free, metadata server will segmentation fault
                             * */ 
                            
                            break;
                        }
                    case SY_CONNECT:
                        {
                            fprintf(stderr, "METADATA_SERVER: receive SY_CONNECT\n");
                            /* TODO: Oauth */

                            char *session_id, *URI_code, *repo_name;
                            struct libwebsocket *fileserver_wsi, *client_wsi;
                            /* lookup the URI_code existed, and get ms_URI_info_t */

                            json_unpack(recvd_session_JData, "{s:s}", "URI_code", &URI_code);
                            struct ms_URI_info_t *URI;
                            /* if the URI_code not exist, send SY_REPO_NOT_EXIST and break */
                            if((URI = get_URI(URI_code)) == NULL)
                            {
                                fprintf(stderr, "METADATA_SERVER: URI %s not exist\n", URI_code);
                                /* TODO: send SY_REPO_NOT_EXIST */
                                break;
                            }

                            fileserver_wsi = URI->fileserver_wsi;
                            client_wsi = wsi;
                            /* Insert (client_wsi, fileserver_wsi, repo_name, state, session_id) into session table */
                            if((session_id = insert_session(fileserver_wsi, client_wsi, URI->repo_name, SY_CONNECT)) == NULL)
                            {
                                /* TODO: if insert failed, what to do? */
                            }


                            /* send SY_CONNECT_OK & session_id  back to client*/
                            json_t *sent_session_JData = json_object();
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(SY_CONNECT_OK));
                            json_object_set_new(sent_session_JData, "session_id", json_string(session_id));
                            char *sent_data_str = json_dumps(sent_session_JData, 0);

                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            write_data->target_wsi = client_wsi;
                            write_data->type = SY_CONNECT_OK;
                            strcpy(write_data->data, sent_data_str);

                            libwebsocket_callback_on_writable(context, wsi);

                            json_decref(recvd_session_JData);   
                            json_decref(sent_session_JData);
                            free(sent_data_str);

                            break;
                        }   
                    case SY_STATUS:
                        {
                            fprintf(stderr, "METADATA_SERVER: receive SY_STATUS\n");
#ifdef DEBUG_META
                            fprintf(stderr, "METADATA_SERVER: receive client checksum\n%s\n\n", recvd_data_str);
#endif
                            /* get session info */
                            char *session_id;
                            json_unpack(recvd_session_JData, "{s:s}", "session_id", &session_id);
                            struct ms_session_info_t *session = get_session(session_id);

                            /* update client checksum in the session table */
                            METADATAtype state = SY_STATUS;
                            json_t *files_json = json_object_get(recvd_session_JData, "files");
                            if(update_session(session_id, "state", (void*)&state) == 0){
                                /* TODO: if update failed, what to do? */
                            }
                            if(update_session(session_id, "client_checksum", (void *)files_json) == 0){
                                /* TODO: if update failed, what to do? */
                            }

                            /* update client_wsi*/
                            if(update_session(session_id, "client_wsi", (void *)wsi) == 0){
                                /* TODO: if update failed, what to do? */
                            }

                            /* send SY_STATUS and session_id repo_name to fileserver to get the file checksum on fileserver */   
                            json_t *sent_session_JData = json_object();
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(SY_STATUS));
                            json_object_set_new(sent_session_JData, "session_id", json_string(session_id));
                            json_object_set_new(sent_session_JData, "repo_name", json_string(session->repo_name));
                            char *sent_data_str = json_dumps(sent_session_JData, 0);
                            
                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            strcpy(write_data->data, sent_data_str);
                            write_data->target_wsi = session->fileserver_wsi;
                            write_data->type = SY_STATUS;
 
                            libwebsocket_callback_on_writable(context, wsi);
 
                            /* free memory */
                            json_decref(recvd_session_JData);   
                            json_decref(sent_session_JData);
                            free(sent_data_str);
                            break; 
                        } 
                    case FS_STATUS_OK:
                        {
                            fprintf(stderr, "METADATA_SERVER: receive fileserver checksum\n%s\n\n", recvd_data_str);
                            /* get session info */
                            char *session_id;
                            json_unpack(recvd_session_JData, "{s:s}", "session_id", &session_id);
                            struct ms_session_info_t *session = get_session(session_id);

                            /* get the fs checksum from the session*/
                            json_t *fs_checksum_array_json = json_object_get(recvd_session_JData, "files");

                            /* get the client checksum from the session*/
                            json_t *client_checksum_array_json = json_loads((const char *)session->files, JSON_DECODE_ANY, &json_err);

                            /* composite client_checksum_json and fs_checksum_json into files_checksum_json */
                            json_t * file_array_json = join_checksum_arrays("filename", fs_checksum_array_json, "fs_checksum", client_checksum_array_json, "client_checksum");
                            json_array_clear(client_checksum_array_json);
                            json_array_clear(fs_checksum_array_json);
                            
                            
                            /* update fs checksum in the field*/
                            if(update_session(session_id, "files", (void *)file_array_json) == 0){
                                /* TODO: if update failed, what to do? */
                            }

                            size_t file_array_size = json_array_size(file_array_json);
                            /* compare fs and client checksum to find out which file is dirty */
                            json_t *file_element_json;
                            size_t file_array_index;
                            const char *filename, *client_checksum, *fs_checksum;

                            json_t *sent_session_JData = json_object();
                            json_t *sent_array = json_array();
                            json_array_foreach(file_array_json, file_array_index, file_element_json){
                                filename = json_string_value(json_object_get(file_element_json, "filename"));
                                client_checksum = json_string_value(json_object_get(file_element_json, "client_checksum"));
                                fs_checksum = json_string_value(json_object_get(file_element_json, "fs_checksum"));

                                json_t *sent_element = json_object();
                                json_object_set_new(sent_element, "filename", json_string(filename));

                                if(strcmp(client_checksum, fs_checksum) == 0)
                                    json_object_set_new(sent_element, "dirty", json_integer(FILE_CLEAN));
                                else if(strcmp(client_checksum, "") == 0)
                                    json_object_set_new(sent_element, "dirty", json_integer(FILE_CLIENT_LACK));
                                else if(strcmp(fs_checksum, "") == 0)
                                    json_object_set_new(sent_element, "dirty", json_integer(FILE_FS_LACK));
                                else
                                    json_object_set_new(sent_element, "dirty", json_integer(FILE_DIRTY));
                                json_array_append(sent_array, sent_element); 
                            }
                            
                            /* update fs checksum in the field*/
                            if(update_session(session_id, "diff", (void *)sent_array) == 0){
                                /* TODO: if update failed, what to do? */
                            }


                            /* send SY_STATUS_OK and a list of dirty file*/
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(SY_STATUS_OK));
                            json_object_set_new(sent_session_JData, "session_id", json_string(session_id));
                            json_object_set_new(sent_session_JData, "files", sent_array);
                            char *sent_data_str = json_dumps(sent_session_JData, 0);
                            
                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            strcpy(write_data->data, sent_data_str);
                            write_data->target_wsi = session->client_wsi;
                            write_data->type = SY_STATUS_OK;
                            
                            libwebsocket_callback_on_writable(context, wsi);
                            
                            /* free memory */
                            json_decref(recvd_session_JData);   
                            json_decref(sent_session_JData);
                            json_decref(file_array_json);
                            free(sent_data_str);
                            break;
                        }
                    case SY_UPLOAD:
                        {
                            fprintf(stderr, "METADATA_SERVER: receive SY_UPLOAD\n");
                            /* get session info */
                            char *session_id, *client_SDP, *client_candidates;
                            json_unpack(recvd_session_JData, "{s:s, s:s, s:s}", 
                                        "session_id", &session_id,
                                        "client_SDP", &client_SDP,
                                        "client_candidates", &client_candidates);

                            struct ms_session_info_t *session = get_session(session_id);
                            /* update client_wsi*/
                            if(update_session(session_id, "client_wsi", (void *)wsi) == 0){
                                /* TODO: if update failed, what to do? */
                            }

                            /* send SY_UPLOAD, repo_name, client_SDP, client_candidate to fileserver */
                            json_t *sent_session_JData = json_object(); 
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(SY_UPLOAD));
                            json_object_set_new(sent_session_JData, "session_id", json_string(session_id));
                            json_object_set_new(sent_session_JData, "repo_name", json_string(session->repo_name));
                            json_object_set_new(sent_session_JData, "client_SDP", json_string(client_SDP));
                            json_object_set_new(sent_session_JData, "client_candidates", json_string(client_candidates));

                            char *sent_data_str = json_dumps(sent_session_JData, 0);
                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            strcpy(write_data->data, sent_data_str);
                            write_data->target_wsi = session->fileserver_wsi;
                            write_data->type = SY_UPLOAD;
                           
                            libwebsocket_callback_on_writable(context, wsi);
                            /* free memory */
                            json_decref(recvd_session_JData);   
                            json_decref(sent_session_JData);
                            free(sent_data_str);
                            break;
                        }
                    case FS_UPLOAD_READY:
                        {
                            fprintf(stderr, "\nMETADATA_SERVER: receive FS_UPLOAD_READY\n");
                            /* get session info */
                            char *session_id;
                            json_unpack(recvd_session_JData, "{s:s}", "session_id", &session_id);
                            struct ms_session_info_t *session = get_session(session_id);
/*
                            json_t *sent_session_JData = json_object(); 
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(FS_UPLOAD_READY));
                            char *sent_data_str = json_dumps(sent_session_JData, 0);
*/
                            /* sent the fs_SDP, fs_candidates to client  */
                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            strcpy(write_data->data, recvd_data_str);
                            write_data->target_wsi = session->client_wsi;
                            write_data->type = FS_UPLOAD_READY;
                            libwebsocket_callback_on_writable(context, wsi);
                            
                            /* free memory */
                            json_decref(recvd_session_JData);   
                            recvd_session_JData=NULL;
                            break;
                        }
                    case SY_DOWNLOAD:
                        {
                            fprintf(stderr, "METADATA_SERVER: receive SY_DOWNLOAD\n");
                            /* get session info */
                            char *session_id, *client_SDP, *client_candidates;
                            json_unpack(recvd_session_JData, "{s:s, s:s, s:s}", 
                                        "session_id", &session_id,
                                        "client_SDP", &client_SDP,
                                        "client_candidates", &client_candidates);

                            struct ms_session_info_t *session = get_session(session_id);
                            /* update client_wsi*/
                            if(update_session(session_id, "client_wsi", (void *)wsi) == 0){
                                /* TODO: if update failed, what to do? */
                            }

                            /* send SY_DOWNLOAD, repo_name, client_SDP, client_candidate to fileserver */
                            json_t *sent_session_JData = json_object(); 
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(SY_DOWNLOAD));
                            json_object_set_new(sent_session_JData, "session_id", json_string(session_id));
                            json_object_set_new(sent_session_JData, "repo_name", json_string(session->repo_name));
                            json_object_set_new(sent_session_JData, "client_SDP", json_string(client_SDP));
                            json_object_set_new(sent_session_JData, "client_candidates", json_string(client_candidates));
                            json_object_set_new(sent_session_JData, "diff", json_loads(session->diff, JSON_DECODE_ANY, NULL));
                         
                            char *sent_data_str = json_dumps(sent_session_JData, 0);
                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            strcpy(write_data->data, sent_data_str);
                            write_data->target_wsi = session->fileserver_wsi;
                            write_data->type = SY_DOWNLOAD;
                           
                            libwebsocket_callback_on_writable(context, wsi);
                            /* free memory */
                            json_decref(recvd_session_JData);   
                            json_decref(sent_session_JData);
                            free(sent_data_str);
                            recvd_session_JData = NULL;
                            sent_session_JData = NULL;
                            sent_data_str = NULL;
                            break;
                        }
                    case FS_DOWNLOAD_READY:
                        {
                            fprintf(stderr, "\nMETADATA_SERVER: receive FS_DOWNLOAD_READY\n");
                            /* get session info */
                            char *session_id;
                            json_unpack(recvd_session_JData, "{s:s}", "session_id", &session_id);
                            struct ms_session_info_t *session = get_session(session_id);
                            
                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            strcpy(write_data->data, recvd_data_str);
                            write_data->target_wsi = session->client_wsi;
                            write_data->type = FS_DOWNLOAD_READY;
                            libwebsocket_callback_on_writable(context, wsi);
                            
                            /* free memory */
                            json_decref(recvd_session_JData);   
                            recvd_session_JData=NULL;

                            break;
                        }
                    case SY_CLEANUP:
                        {
                            fprintf(stderr, "METADATA_SERVER: receive SY_CLEANUP\n");
                            char *session_id;
                            json_unpack(recvd_session_JData, "{s:s}", 
                                        "session_id", &session_id);

                            /* remove the session in mongodb */
                            delete_session(session_id); 

                            /* send SY_CLEANUP_OK back */
                            json_t *sent_session_JData = json_object(); 
                            json_object_set_new(sent_session_JData, "metadata_type", json_integer(SY_CLEANUP_OK));
                         
                            char *sent_data_str = json_dumps(sent_session_JData, 0);
                            struct lwst_writedata_t *write_data = *write_data_ptr;
                            strcpy(write_data->data, sent_data_str);
                            write_data->target_wsi = wsi;
                            write_data->type = SY_CLEANUP_OK;
                           
                            libwebsocket_callback_on_writable(context, wsi);
                            /* free memory */
                            json_decref(recvd_session_JData);   
                            json_decref(sent_session_JData);
                            free(sent_data_str);
                            recvd_session_JData = NULL;
                            sent_session_JData = NULL;
                            sent_data_str = NULL;
                            break;
                        }
                    default:
                        break;
                }
                break;
            }
        case LWS_CALLBACK_SERVER_WRITEABLE:
            {
                int lws_err;
                size_t data_len;
                struct lwst_writedata_t *write_data = *write_data_ptr;
                if((data_len = strlen(write_data->data)) >0 )
                {   
                    lws_err = libwebsocket_write(write_data->target_wsi, (void *)write_data->data, data_len, LWS_WRITE_TEXT);
#ifdef DEBUG_META
                    fprintf(stderr, "METADATA_SERVER: write data:%s,%d\n", write_data->data, data_len);
#endif
                    switch(write_data->type){
                        case(FS_REGISTER_OK_t):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send FS_REGISTER_OK_t fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send FS_REGISTER_OK_t\n");
#endif
                                break; 
                            }
                        case(SY_INIT):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send SY_INIT to fileserver fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send SY_INIT to fileserver\n");
#endif
                                break;
                            }
                        case(SY_INIT_OK):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send SY_INIT_OK to client fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send SY_INIT_OK to client\n");
#endif
                                break;
                            }
                        case(SY_CONNECT_OK):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send SY_CONNECT_OK to client fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send SY_CONNECT_OK to client\n");
#endif
                                break;
                            }
                        case(SY_STATUS):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send SY_STATUS to fileserver fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send SY_STATUS to fileserver\n");
#endif
                                break;
                            }
                        case(SY_STATUS_OK):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send SY_STATUS_OK to client fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send SY_STATUS_OK to client\n");
#endif
                                break;
                            }
                        case(SY_UPLOAD):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send SY_UPLOAD to fileserver fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send SY_UPLOAD to fileserver\n");
#endif
                                break;
                            }
                        case(FS_UPLOAD_READY):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send SY_UPLOAD_READY to client fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send SY_UPLOAD_READY to client\n");
#endif
                                break;
                            }
                        case(SY_DOWNLOAD):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send SY_DOWNLOAD to fileserver fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send SY_DOWNLOAD to fileserver\n");
#endif
                                break;
                            }
                        case(FS_DOWNLOAD_READY):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send FS_DOWNLOAD_READY to client fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send FS_DOWNLOAD_READY to client\n");
#endif
                                break;
                            }
                        case(SY_CLEANUP_OK):
                            {
#ifdef DEBUG_META
                                if(lws_err<0)
                                    fprintf(stderr, "METADATA_SERVER: send SY_CLEANUP_OK to client fail\n");
                                else
                                    fprintf(stderr, "METADATA_SERVER: send SY_CLEANUP_OK to client\n");
#endif
                                break;
                            }
                        default:
                            break;
                    }
                }
                memset((write_data), 0, sizeof(write_data));
                break;
            }
        default:
            break;
    }
    return 0;
}


static struct libwebsocket_protocols protocols[] = {
    {
        "http-only",
        callback_http,
        0
    },
    {
        "fileserver-protocol",
        callback_SDP,
        2048,
    },
    {
        "client-protocol",
        callback_SDP,
        2048,
        0
    },
    { NULL, NULL, 0, 0 }
};

void sighandler(int sig)
{
    force_exit = 1;
    libwebsocket_cancel_service(context);
}


int main()
{
    char cert_path[1024];
    char key_path[1024];
    int use_ssl = 0;
    int debug_level = 7;
    const char *iface = NULL;
    struct lws_context_creation_info info;
    int n;
    int opts = 0;
    int daemonize = 0;
    int syslog_options = LOG_PID | LOG_PERROR; 

    memset(&info, 0, sizeof(info));
    info.port = 7681;

    signal(SIGINT, sighandler);

    setlogmask(LOG_UPTO (LOG_DEBUG));
    openlog("lwsts", syslog_options, LOG_DAEMON);
    lws_set_log_level(debug_level, lwsl_emit_syslog);


    info.iface = iface;
    info.protocols = protocols;
    info.extensions = libwebsocket_get_internal_extensions();

    info.ssl_cert_filepath = NULL;
    info.ssl_private_key_filepath = NULL;

    info.gid = -1;
    info.uid = -1;
    info.options = opts;

    context = libwebsocket_create_context(&info);

    if (context == NULL) {
        lwsl_err("create context failed\n");
        return -1;
    }
    n = 0;


    /* initial the connection to mongodb */
    mongoc_init ();
    client = mongoc_client_new ("mongodb://localhost:27017/");
    fileserver_coll = mongoc_client_get_collection (client, "metadata", "fileserver");
    session_coll = mongoc_client_get_collection (client, "metadata", "session");
    filesys_coll = mongoc_client_get_collection (client, "metadata", "filesys");
    while( n>=0 && !force_exit){
        n = libwebsocket_service(context, 50);
    }

    libwebsocket_context_destroy(context); 
    lwsl_notice("libwebsockets-test-server exited cleanly\n"); 

    closelog();

    return 0;
}
