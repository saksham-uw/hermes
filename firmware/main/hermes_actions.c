#include "hermes_actions.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "hermes_actions_config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "hermes_act";
#define RESPONSE_CAP 8192

static void copy_text(char *dest, size_t dest_len, const char *src);
static esp_err_t map_error_body(const char *body, int status, char *err,
                               size_t err_len);

typedef struct {
  char *data;
  int cap;
  int length;
} response_buf_t;

bool hermes_actions_configured(void) {
  return HERMES_ACTIONS_URL[0] != 0 && HERMES_ACTIONS_DEVICE_TOKEN[0] != 0;
}

void hermes_actions_make_key(char *out, size_t out_len) {
  if (!out || out_len < 17)
    return;
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  uint32_t c = esp_random();
  uint32_t d = esp_random();
  snprintf(out, out_len, "%08lx%08lx%08lx%08lx", (unsigned long)a,
           (unsigned long)b, (unsigned long)c, (unsigned long)d);
}

static void copy_text(char *dest, size_t dest_len, const char *src) {
  if (!dest || dest_len == 0)
    return;
  if (!src) {
    dest[0] = 0;
    return;
  }
  snprintf(dest, dest_len, "%s", src);
}

static const char *json_string(cJSON *object, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

static bool json_bool(cJSON *object, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsTrue(item);
}

static int json_int(cJSON *object, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsNumber(item) ? item->valueint : 0;
}

static esp_err_t map_error_body(const char *body, int status, char *err,
                               size_t err_len) {
  cJSON *root = cJSON_Parse(body);
  char code[32] = {0};
  char message[181] = {0};
  if (root) {
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
      copy_text(code, sizeof(code), json_string(error, "code"));
      copy_text(message, sizeof(message), json_string(error, "message"));
    }
    cJSON_Delete(root);
  }
  if (strcmp(code, "device_auth") == 0) {
    copy_text(err, err_len, "device auth failed");
    return ESP_ERR_INVALID_STATE;
  }
  if (strcmp(code, "github_auth") == 0 &&
      strstr(message, "accessible by personal access token")) {
    copy_text(err, err_len, "actions access pending");
    return ESP_ERR_INVALID_STATE;
  }
  if (status <= 0)
    copy_text(err, err_len, "service offline");
  else if (message[0])
    copy_text(err, err_len, message);
  else
    copy_text(err, err_len, "service offline");
  return ESP_FAIL;
}

static esp_err_t http_event(esp_http_client_event_t *evt) {
  response_buf_t *buf = (response_buf_t *)evt->user_data;
  if (evt->event_id != HTTP_EVENT_ON_DATA || !buf || !evt->data)
    return ESP_OK;
  int copy = evt->data_len;
  if (buf->length + copy >= buf->cap)
    copy = buf->cap - 1 - buf->length;
  if (copy > 0) {
    memcpy(buf->data + buf->length, evt->data, (size_t)copy);
    buf->length += copy;
    buf->data[buf->length] = 0;
  }
  return ESP_OK;
}

static esp_err_t perform(const char *path, const char *method,
                         const char *idempotency_key, const char *json_body,
                         char *response, int response_cap, int *status_out,
                         char *err, size_t err_len) {
  if (!hermes_actions_configured()) {
    copy_text(err, err_len, "service offline");
    return ESP_ERR_INVALID_STATE;
  }
  char url[256];
  snprintf(url, sizeof(url), "%s%s", HERMES_ACTIONS_URL, path);
  response[0] = 0;
  response_buf_t buf = {.data = response, .cap = response_cap, .length = 0};
  esp_http_client_config_t config = {
      .url = url,
      .timeout_ms = 25000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 2048,
      .buffer_size_tx = 1024,
      .event_handler = http_event,
      .user_data = &buf,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    copy_text(err, err_len, "service offline");
    return ESP_FAIL;
  }
  if (strcmp(method, "POST") == 0)
    esp_http_client_set_method(client, HTTP_METHOD_POST);
  else
    esp_http_client_set_method(client, HTTP_METHOD_GET);
  char auth[160];
  snprintf(auth, sizeof(auth), "Bearer %s", HERMES_ACTIONS_DEVICE_TOKEN);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "Accept", "application/json");
  if (idempotency_key && idempotency_key[0])
    esp_http_client_set_header(client, "Idempotency-Key", idempotency_key);
  if (json_body) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));
  }
  esp_err_t err_code = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  if (status_out)
    *status_out = status;
  esp_http_client_cleanup(client);
  if (err_code != ESP_OK) {
    ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(err_code));
    copy_text(err, err_len, "service offline");
    return ESP_FAIL;
  }
  if (status < 200 || status >= 300)
    return map_error_body(response, status, err, err_len);
  return ESP_OK;
}

static esp_err_t perform_json(const char *path, const char *method,
                              const char *idempotency_key,
                              const char *json_body, cJSON **root_out,
                              char *err, size_t err_len) {
  char *response = calloc(1, RESPONSE_CAP);
  if (!response) {
    copy_text(err, err_len, "out of memory");
    return ESP_ERR_NO_MEM;
  }
  int status = 0;
  esp_err_t result = perform(path, method, idempotency_key, json_body, response,
                             RESPONSE_CAP, &status, err, err_len);
  if (result == ESP_OK) {
    *root_out = cJSON_Parse(response);
    if (!*root_out) {
      copy_text(err, err_len, "invalid service response");
      result = ESP_ERR_INVALID_RESPONSE;
    }
  }
  free(response);
  return result;
}

static void fill_pull(cJSON *object, hermes_git_pull_t *out) {
  memset(out, 0, sizeof(*out));
  out->number = json_int(object, "number");
  copy_text(out->title, sizeof(out->title), json_string(object, "title"));
  copy_text(out->head, sizeof(out->head), json_string(object, "head"));
  copy_text(out->base, sizeof(out->base), json_string(object, "base"));
  copy_text(out->head_sha, sizeof(out->head_sha), json_string(object, "headSha"));
  copy_text(out->mergeable_state, sizeof(out->mergeable_state),
            json_string(object, "mergeableState"));
  out->ready = json_bool(object, "ready");
  cJSON *approved = cJSON_GetObjectItemCaseSensitive(object, "approvedBy");
  out->approved_by[0] = 0;
  if (cJSON_IsArray(approved)) {
    int count = cJSON_GetArraySize(approved);
    for (int i = 0; i < count; i++) {
      cJSON *item = cJSON_GetArrayItem(approved, i);
      if (!cJSON_IsString(item))
        continue;
      if (out->approved_by[0])
        strlcat(out->approved_by, ",", sizeof(out->approved_by));
      strlcat(out->approved_by, item->valuestring, sizeof(out->approved_by));
    }
  }
}

static void fill_creation(cJSON *root, hermes_git_creation_t *out) {
  memset(out, 0, sizeof(*out));
  out->created = json_bool(root, "created");
  cJSON *pull = cJSON_GetObjectItemCaseSensitive(root, "pull");
  if (cJSON_IsObject(pull))
    fill_pull(pull, &out->pull);
  cJSON *warnings = cJSON_GetObjectItemCaseSensitive(root, "warnings");
  if (cJSON_IsArray(warnings) && cJSON_GetArraySize(warnings) > 0) {
    cJSON *first = cJSON_GetArrayItem(warnings, 0);
    if (cJSON_IsString(first))
      copy_text(out->warnings, sizeof(out->warnings), first->valuestring);
  }
}

static int64_t json_epoch(cJSON *object, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsNumber(item) ? (int64_t)item->valuedouble : 0;
}

static void fill_deployment(cJSON *object, hermes_git_deployment_t *out) {
  memset(out, 0, sizeof(*out));
  if (!cJSON_IsObject(object))
    return;
  out->valid = true;
  copy_text(out->state, sizeof(out->state), json_string(object, "state"));
  copy_text(out->candidate_tag, sizeof(out->candidate_tag),
            json_string(object, "candidateTag"));
  copy_text(out->latest_tag, sizeof(out->latest_tag),
            json_string(object, "latestTag"));
  out->run_number = json_int(object, "runNumber");
  out->latest_run_number = json_int(object, "latestRunNumber");
  out->started_at = json_epoch(object, "startedAtEpoch");
  out->completed_at = json_epoch(object, "completedAtEpoch");
}

esp_err_t hermes_actions_health(char *err, size_t err_len) {
  cJSON *root = NULL;
  esp_err_t result = perform_json("/v1/health", "GET", NULL, NULL, &root,
                                  err, err_len);
  if (result != ESP_OK)
    return result;
  bool ok = root && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"));
  cJSON_Delete(root);
  if (!ok) {
    copy_text(err, err_len, "service offline");
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t hermes_actions_list_branches(const char *repo,
                                       hermes_git_branch_t *out, int *count,
                                       char *err, size_t err_len) {
  char path[96];
  snprintf(path, sizeof(path), "/v1/git/branches?recent_hours=48&repo=%s",
           repo);
  *count = 0;
  cJSON *root = NULL;
  esp_err_t result =
      perform_json(path, "GET", NULL, NULL, &root, err, err_len);
  if (result != ESP_OK)
    return result;
  cJSON *branches =
      root ? cJSON_GetObjectItemCaseSensitive(root, "branches") : NULL;
  if (!cJSON_IsArray(branches)) {
    cJSON_Delete(root);
    copy_text(err, err_len, "service offline");
    return ESP_FAIL;
  }
  int n = cJSON_GetArraySize(branches);
  if (n > HERMES_GIT_MAX_BRANCHES)
    n = HERMES_GIT_MAX_BRANCHES;
  for (int i = 0; i < n; i++) {
    cJSON *item = cJSON_GetArrayItem(branches, i);
    memset(&out[i], 0, sizeof(out[i]));
    copy_text(out[i].name, sizeof(out[i].name), json_string(item, "name"));
    copy_text(out[i].sha, sizeof(out[i].sha), json_string(item, "sha"));
  }
  *count = n;
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t hermes_actions_list_pulls(const char *repo, hermes_git_pull_t *out,
                                      int *count, char *err, size_t err_len) {
  char path[80];
  snprintf(path, sizeof(path), "/v1/git/pulls?repo=%s", repo);
  *count = 0;
  cJSON *root = NULL;
  esp_err_t result =
      perform_json(path, "GET", NULL, NULL, &root, err, err_len);
  if (result != ESP_OK)
    return result;
  cJSON *pulls = root ? cJSON_GetObjectItemCaseSensitive(root, "pulls") : NULL;
  if (!cJSON_IsArray(pulls)) {
    cJSON_Delete(root);
    copy_text(err, err_len, "service offline");
    return ESP_FAIL;
  }
  int n = cJSON_GetArraySize(pulls);
  if (n > HERMES_GIT_MAX_PULLS)
    n = HERMES_GIT_MAX_PULLS;
  for (int i = 0; i < n; i++)
    fill_pull(cJSON_GetArrayItem(pulls, i), &out[i]);
  *count = n;
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t hermes_actions_list_deployments(hermes_git_deployment_t *out,
                                          int *count, char *err,
                                          size_t err_len) {
  *count = 0;
  cJSON *root = NULL;
  esp_err_t result = perform_json("/v1/git/deployments", "GET", NULL, NULL,
                                  &root, err, err_len);
  if (result != ESP_OK)
    return result;
  cJSON *deployments =
      root ? cJSON_GetObjectItemCaseSensitive(root, "deployments") : NULL;
  if (!cJSON_IsArray(deployments)) {
    cJSON_Delete(root);
    copy_text(err, err_len, "invalid deployment data");
    return ESP_ERR_INVALID_RESPONSE;
  }
  int available = cJSON_GetArraySize(deployments);
  if (available > 2)
    available = 2;
  for (int index = 0; index < available; index++)
    fill_deployment(cJSON_GetArrayItem(deployments, index), &out[index]);
  *count = available;
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t hermes_actions_create_prod(const char *repo,
                                      const char *idempotency_key,
                                      hermes_git_creation_t *out, char *err,
                                      size_t err_len) {
  char body[80];
  snprintf(body, sizeof(body), "{\"repo\":\"%s\"}", repo);
  cJSON *root = NULL;
  esp_err_t result = perform_json("/v1/git/workflows/prod-from-main/pulls",
                                  "POST", idempotency_key, body, &root, err,
                                  err_len);
  if (result != ESP_OK)
    return result;
  fill_creation(root, out);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t hermes_actions_create_from_branch(const char *repo, const char *branch,
                                           const char *idempotency_key,
                                           hermes_git_creation_t *out,
                                           char *err, size_t err_len) {
  char body[160];
  snprintf(body, sizeof(body), "{\"repo\":\"%s\",\"branch\":\"%s\"}", repo,
           branch);
  cJSON *root = NULL;
  esp_err_t result = perform_json(
      "/v1/git/workflows/main-from-branch/pulls", "POST", idempotency_key,
      body, &root, err, err_len);
  if (result != ESP_OK)
    return result;
  fill_creation(root, out);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t hermes_actions_merge(const char *repo, int number, const char *head_sha,
                               const char *idempotency_key,
                               hermes_git_merge_t *out, char *err,
                               size_t err_len) {
  char path[48];
  char body[160];
  snprintf(path, sizeof(path), "/v1/git/pulls/%d/merge", number);
  snprintf(body, sizeof(body), "{\"repo\":\"%s\",\"headSha\":\"%s\"}", repo,
           head_sha);
  cJSON *root = NULL;
  esp_err_t result = perform_json(path, "POST", idempotency_key, body, &root,
                                  err, err_len);
  if (result != ESP_OK)
    return result;
  memset(out, 0, sizeof(*out));
  out->merged = json_bool(root, "merged");
  copy_text(out->sha, sizeof(out->sha), json_string(root, "sha"));
  copy_text(out->message, sizeof(out->message), json_string(root, "message"));
  cJSON_Delete(root);
  return ESP_OK;
}
