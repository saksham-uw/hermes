#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HERMES_GIT_MAX_BRANCHES 8
#define HERMES_GIT_MAX_PULLS 8
#define HERMES_GIT_NAME_LEN 40
#define HERMES_GIT_TITLE_LEN 48
#define HERMES_GIT_SHA_LEN 44
#define HERMES_GIT_MSG_LEN 72
#define HERMES_GIT_TAG_LEN 20

typedef struct {
  char name[HERMES_GIT_NAME_LEN];
  char sha[16];
} hermes_git_branch_t;

typedef struct {
  int number;
  char title[HERMES_GIT_TITLE_LEN];
  char head[HERMES_GIT_NAME_LEN];
  char base[16];
  char head_sha[HERMES_GIT_SHA_LEN];
  char mergeable_state[16];
  char approved_by[64];
  bool ready;
} hermes_git_pull_t;

typedef struct {
  hermes_git_pull_t pull;
  bool created;
  char warnings[HERMES_GIT_MSG_LEN];
} hermes_git_creation_t;

typedef struct {
  bool merged;
  char sha[HERMES_GIT_SHA_LEN];
  char message[HERMES_GIT_MSG_LEN];
} hermes_git_merge_t;

typedef struct {
  bool valid;
  char state[16];
  char candidate_tag[HERMES_GIT_TAG_LEN];
  char latest_tag[HERMES_GIT_TAG_LEN];
  int run_number;
  int latest_run_number;
  int64_t started_at;
  int64_t completed_at;
} hermes_git_deployment_t;

bool hermes_actions_configured(void);
void hermes_actions_make_key(char *out, size_t out_len);

esp_err_t hermes_actions_health(char *err, size_t err_len);
esp_err_t hermes_actions_list_branches(const char *repo,
                                       hermes_git_branch_t *out, int *count,
                                       char *err, size_t err_len);
esp_err_t hermes_actions_list_pulls(const char *repo, hermes_git_pull_t *out,
                                      int *count, char *err, size_t err_len);
esp_err_t hermes_actions_create_prod(const char *repo,
                                      const char *idempotency_key,
                                      hermes_git_creation_t *out, char *err,
                                      size_t err_len);
esp_err_t hermes_actions_list_deployments(hermes_git_deployment_t *out,
                                          int *count, char *err,
                                          size_t err_len);
esp_err_t hermes_actions_create_from_branch(const char *repo, const char *branch,
                                           const char *idempotency_key,
                                           hermes_git_creation_t *out,
                                           char *err, size_t err_len);
esp_err_t hermes_actions_merge(const char *repo, int number, const char *head_sha,
                               const char *idempotency_key,
                               hermes_git_merge_t *out, char *err,
                               size_t err_len);
