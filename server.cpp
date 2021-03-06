#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdarg>
#include <csignal>
#include <cstring>
#include <cctype>
#include <ctime>
#include <cmath>

#include <vector>
#include <list>
#include <set>

#include "constants.h"
#include "server.h"
#include "common.h"
#include "func.h"

#define REGISTERED_USER_LIST_SIZE 100

#define REGISTERED_USER_FILE "userlists.log"

using std::multiset;
using std::vector;

pthread_mutex_t userlist_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t battles_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t default_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t items_lock[USER_CNT];

int server_fd = 0, port = 50000, port_range = 100;

void wrap_recv(int conn, client_message_t* pcm);
void wrap_send(int conn, server_message_t* psm);

void send_to_client(int uid, int message);
void send_to_client(int uid, int message, char* str);
void say_to_client(int uid, char* message);
void send_to_client_with_username(int uid, int message, char* user_name);
void close_session(int conn, int message);

void check_user_status(int uid);

void terminate_process(int recved_signal);

static int user_list_size = 0;
//static uint64_t sum_delay_time = 0, prev_time;

struct {
    char user_name[USERNAME_SIZE];
    char password[PASSWORD_SIZE];
} registered_user_list[REGISTERED_USER_LIST_SIZE];

struct session_t {
    char user_name[USERNAME_SIZE];
    char ip_addr[IPADDR_SIZE];
    int conn;
    int state;
    int is_admin;
    int score;
    int kill;
    int death;
    uint32_t bid;
    uint32_t inviter_id;
    client_message_t cm;
} sessions[USER_CNT];

struct session_args_t {
    int conn;
    char ip_addr[IPADDR_SIZE];
};

typedef struct session_args_t session_args_t;

class item_t { public:
    int id;
    int dir;
    int owner;
    uint64_t time;
    int count;
    int kind;
    pos_t pos;
    item_t(const item_t &it) : id(it.id),
                               dir(it.dir),
                               owner(it.owner),
                               time(it.time),
                               count(it.count),
                               kind(it.kind),
                               pos(it.pos)
                               {}
    item_t() {
        id = 0;
        dir = owner = time = count = kind = 0;
        pos.x = pos.y = 0;
    }
    friend const bool operator < (const item_t it1, const item_t it2) {
        return it1.time < it2.time;
    }
};

class battle_t { public:
    int is_alloced;
    size_t alive_users;
    size_t all_users;
    class user_t { public:
        int battle_state;
        int energy;
        int dir;
        int life;
        int killby;
        pos_t pos;
        pos_t last_pos;
    } users[USER_CNT];

    int num_of_other;  // number of other alloced item except for bullet
    int item_count;
    uint64_t global_time;

    std::list<item_t> items;

    void reset() {
        is_alloced = all_users = alive_users = num_of_other = item_count = 0;
        global_time = 0;
        items.clear();
    }
    battle_t() {
        reset();
    }

} battles[USER_CNT];

void load_user_list() {
    FILE* userlist = fopen(REGISTERED_USER_FILE, "r");
    if (userlist == NULL) {
        log("can not find " REGISTERED_USER_FILE "");
        return;
    }
#define LOAD_FAIL                                                                          \
    log("failed to load users, try to delete " REGISTERED_USER_FILE "."),                \
        user_list_size = 0, memset(registered_user_list, 0, sizeof(registered_user_list)), \
        fclose(userlist);
    for (int i = 0; i < REGISTERED_USER_LIST_SIZE; i++) {
        if (fgets(registered_user_list[i].user_name, USERNAME_SIZE, userlist) != NULL) {
            registered_user_list[i].user_name[strlen(registered_user_list[i].user_name) - 1] = 0;
            for (int j = 0; j < i; j++) {
                if (strncmp(registered_user_list[i].user_name, registered_user_list[j].user_name, USERNAME_SIZE - 1) != 0)
                    continue;
                LOAD_FAIL;
                return;
            }
            user_list_size++;
        } else {
            break;
        }
        if (fgets(registered_user_list[i].password, PASSWORD_SIZE, userlist) == NULL) {
            LOAD_FAIL;
            return;
        }
        registered_user_list[i].password[strlen(registered_user_list[i].password) - 1] = 0;
    }
#undef LOAD_FAIL
    //for (int i = 0; i < user_list_size; i++) {
    //    log("loaded user %s", registered_user_list[i].user_name);
    //}
    log("loaded %d user(s) from " REGISTERED_USER_FILE ".", user_list_size);
    fclose(userlist);
}
void save_user_list() {
    FILE* userlist = fopen(REGISTERED_USER_FILE, "w");
    for (int i = 0; i < user_list_size; i++) {
        fprintf(userlist, "%s\n", registered_user_list[i].user_name);
        fprintf(userlist, "%s\n", registered_user_list[i].password);
    }
    log("saved %d users to " REGISTERED_USER_FILE ".", user_list_size);
    fclose(userlist);
}

void save_user(int i) {
    FILE* userlist = fopen(REGISTERED_USER_FILE, "a");
    fprintf(userlist, "%s\n", registered_user_list[i].user_name);
    fprintf(userlist, "%s\n", registered_user_list[i].password);
    log("saved users %s to " REGISTERED_USER_FILE ".", registered_user_list[i].user_name);
    fclose(userlist);
}

int query_session_built(uint32_t uid) {
    assert(uid < USER_CNT);

    if (sessions[uid].state == USER_STATE_UNUSED
        || sessions[uid].state == USER_STATE_NOT_LOGIN) {
        return false;
    } else {
        return true;
    }
}

void inform_all_user_battle_player(int bid);

void user_quit_battle(uint32_t bid, uint32_t uid) {
    assert(bid < USER_CNT && uid < USER_CNT);

    log("user %s\033[2m(%s)\033[0m quit from battle %d(%ld/%ld users)", sessions[uid].user_name, sessions[uid].ip_addr, bid, battles[bid].alive_users, battles[bid].all_users);
    battles[bid].all_users--;
    if (battles[bid].users[uid].battle_state == BATTLE_STATE_LIVE) {
        battles[bid].alive_users--;
        if (battles[bid].alive_users != 0) {
            sessions[uid].score = max(sessions[uid].score - 5, 0);
            sessions[uid].death ++;
        }
    }
    battles[bid].users[uid].battle_state = BATTLE_STATE_UNJOINED;
    sessions[uid].state = USER_STATE_LOGIN;
    if (battles[bid].all_users == 0) {
        // disband battle
        log("disband battle %d", bid);
        battles[bid].reset();
    } else {
        server_message_t sm;
        sm.message = SERVER_MESSAGE_USER_QUIT_BATTLE;
        strncpy(sm.friend_name, sessions[uid].user_name, USERNAME_SIZE - 1);

        for (int i = 0; i < USER_CNT; i++) {
            if (battles[bid].users[i].battle_state != BATTLE_STATE_UNJOINED) {
                wrap_send(sessions[i].conn, &sm);
            }
        }
    }
}

void user_join_battle_common_part(uint32_t bid, uint32_t uid, uint32_t joined_state) {
    log("user %s\033[2m(%s)\033[0m join in battle %d", sessions[uid].user_name, sessions[uid].ip_addr, bid);

    if (joined_state == USER_STATE_BATTLE) {
        battles[bid].all_users++;
        battles[bid].alive_users++;
        log("now %ld alive of %ld users", battles[bid].alive_users, battles[bid].all_users);
        battles[bid].users[uid].battle_state = BATTLE_STATE_LIVE;
    } else if (joined_state == USER_STATE_WAIT_TO_BATTLE) {
        battles[bid].users[uid].battle_state = BATTLE_STATE_UNJOINED;
    } else {
        loge("check here, other joined_state:%d", joined_state);
    }

    battles[bid].users[uid].life = INIT_LIFE;
    battles[bid].users[uid].energy = INIT_BULLETS;

    sessions[uid].state = joined_state;
    sessions[uid].bid = bid;
}

void user_join_battle(uint32_t bid, uint32_t uid) {
    int ux = (rand() & 0x7FFF) % BATTLE_W;
    int uy = (rand() & 0x7FFF) % BATTLE_H;
    battles[bid].users[uid].pos.x = ux;
    battles[bid].users[uid].pos.y = uy;
    log("alloc position (%hhu, %hhu) for launcher #%d %s",
        ux, uy, uid, sessions[uid].user_name);

    sessions[uid].state = USER_STATE_BATTLE;

    if (battles[bid].users[uid].battle_state == BATTLE_STATE_UNJOINED) {
        user_join_battle_common_part(bid, uid, USER_STATE_BATTLE);
    }
}

void user_invited_to_join_battle(uint32_t bid, uint32_t uid) {
    if (sessions[uid].state == USER_STATE_WAIT_TO_BATTLE
        && bid != sessions[uid].bid) {
        log("user #%d %s\033[2m(%s)\033[0m rejects old battle #%d since he was invited to a new battle",
            uid, sessions[uid].user_name, sessions[uid].ip_addr, sessions[uid].bid);

        send_to_client_with_username(sessions[uid].inviter_id, SERVER_MESSAGE_FRIEND_REJECT_BATTLE, sessions[uid].user_name);
    }

    user_join_battle_common_part(bid, uid, USER_STATE_WAIT_TO_BATTLE);
}

int find_uid_by_user_name(const char* user_name) {
    int ret_uid = -1;
    log("find user %s", user_name);
    for (int i = 0; i < USER_CNT; i++) {
        if (query_session_built(i)) {
            if (strncmp(user_name, sessions[i].user_name, USERNAME_SIZE - 1) == 0) {
                ret_uid = i;
                break;
            }
        }
    }

    if (ret_uid == -1) {
        logi("fail");
    } else {
        logi("found: #%d %s\033[2m(%s)\033[0m", ret_uid, sessions[ret_uid].user_name, sessions[ret_uid].ip_addr);
    }

    return ret_uid;
}

int get_unalloced_battle() {
    int ret_bid = -1;
    pthread_mutex_lock(&battles_lock);
    for (int i = 1; i < USER_CNT; i++) {
        if (battles[i].is_alloced == false) {
            battles[i].reset();
            battles[i].is_alloced = true;
            ret_bid = i;
            break;
        }
    }
    pthread_mutex_unlock(&battles_lock);
    if (ret_bid == -1) {
        loge("check here, returned battle id should not be -1");
    } else {
        log("alloc unalloced battle id #%d", ret_bid);
    }
    return ret_bid;
}

int get_unused_session() {
    int ret_uid = -1;
    pthread_mutex_lock(&sessions_lock);
    for (int i = 0; i < USER_CNT; i++) {
        if (sessions[i].state == USER_STATE_UNUSED) {
            memset(&sessions[i], 0, sizeof(struct session_t));
            sessions[i].conn = -1;
            sessions[i].state = USER_STATE_NOT_LOGIN;
            ret_uid = i;
            break;
        }
    }
    pthread_mutex_unlock(&sessions_lock);
    if (ret_uid == -1) {
        log("fail to alloc session id");
    } else {
        log("alloc unused session id #%d", ret_uid);
    }
    return ret_uid;
}

void inform_friends(int uid, int message) {
    server_message_t sm;
    char* user_name = sessions[uid].user_name;
    memset(&sm, 0, sizeof(server_message_t));
    sm.message = message;
    for (int i = 0; i < USER_CNT; i++) {
        if (i == uid || !query_session_built(i))
            continue;
        strncpy(sm.friend_name, user_name, USERNAME_SIZE - 1);
        wrap_send(sessions[i].conn, &sm);
    }
}

void forced_generate_items(int bid, int x, int y, int kind, int count, int uid = -1) {
    //if (battles[bid].num_of_other >= MAX_OTHER) return;
    if (x < 0 || x >= BATTLE_W) return;
    if (y < 0 || y >= BATTLE_H) return;
    battles[bid].item_count++;
    item_t new_item;
    new_item.id = battles[bid].item_count;
    new_item.kind = kind;
    new_item.pos.x = x;
    new_item.pos.y = y;
    new_item.time = battles[bid].global_time + count;
    new_item.owner = uid;
    if (kind == ITEM_MAGMA) {
        new_item.count = MAGMA_INIT_TIMES;
    }
    battles[bid].items.push_back(new_item);
    log("new %s #%d (%d,%d)",
        item_s[new_item.kind],
        new_item.id,
        new_item.pos.x,
        new_item.pos.y);
}

void random_generate_items(int bid) {
    int random_kind;
    if (!probability(1, 100)) return;
    if (battles[bid].num_of_other >= MAX_OTHER) return;
    random_kind = rand() % (ITEM_END - 1) + 1;
    if (random_kind == ITEM_BLOOD_VIAL && probability(1, 2))
        random_kind = ITEM_MAGAZINE;
    battles[bid].item_count++;
    item_t new_item;
    new_item.id = battles[bid].item_count;
    new_item.kind = random_kind;
    new_item.pos.x = (rand() & 0x7FFF) % BATTLE_W;
    new_item.pos.y = (rand() & 0x7FFF) % BATTLE_H;
    new_item.time = battles[bid].global_time + OTHER_ITEM_LASTS_TIME;
    battles[bid].num_of_other++;
    log("new %s #%d (%d,%d)",
        item_s[new_item.kind],
        new_item.id,
        new_item.pos.x,
        new_item.pos.y);
    if (random_kind == ITEM_MAGMA) {
        new_item.count = MAGMA_INIT_TIMES;
    }
    battles[bid].items.push_back(new_item);
    //for (int i = 0; i < USER_CNT; i++) {
    //    if (battles[bid].users[i].battle_state != BATTLE_STATE_LIVE)
    //        continue;
    //    check_user_status(i);
    //}
}

void move_bullets(int bid) {
    for (auto& cur : battles[bid].items) {
    //for (int i = 0; i < MAX_ITEM; i++) {
        if (cur.kind != ITEM_BULLET)
            continue;
        // log("try to move bullet %d with dir %d", i, cur.dir);
        switch (cur.dir) {
            case DIR_UP: {
                if (cur.pos.y > 0) { (cur.pos.y)--; break; }
                else { cur.dir = DIR_DOWN; break;}
            }
            case DIR_DOWN: {
                if (cur.pos.y < BATTLE_H - 1) { (cur.pos.y)++; break; }
                else { cur.dir = DIR_UP; break;}
            }
            case DIR_LEFT: {
                if (cur.pos.x > 0) { (cur.pos.x)--; break; }
                else { cur.dir = DIR_RIGHT; break;}
            }
            case DIR_RIGHT: {
                if (cur.pos.x < BATTLE_W - 1) { (cur.pos.x)++; break; }
                else { cur.dir = DIR_LEFT; break; }
            }
            case DIR_UP_LEFT: {
                if (cur.pos.y > 0) { (cur.pos.y)--; }
                else { cur.dir = DIR_DOWN_LEFT; break; }
                if (cur.pos.x > 1) { (cur.pos.x) -= 2; }
                else { cur.dir = DIR_UP_RIGHT; break; }
                break;
            }
            case DIR_UP_RIGHT: {
                if (cur.pos.y > 0) { (cur.pos.y)--; }
                else { cur.dir = DIR_DOWN_RIGHT; break;}
                if (cur.pos.x < BATTLE_W - 2) { (cur.pos.x) += 2; }
                else { cur.dir = DIR_UP_LEFT; break; }
                break;
            }
            case DIR_DOWN_LEFT: {
                if (cur.pos.y < BATTLE_H - 2) { (cur.pos.y)++; }
                else { cur.dir = DIR_UP_LEFT; break; }
                if (cur.pos.x > 1) { (cur.pos.x) -= 2; }
                else { cur.dir = DIR_DOWN_RIGHT; break; }
                break;
            }
            case DIR_DOWN_RIGHT: {
                if (cur.pos.y < BATTLE_H - 2) { (cur.pos.y)++; }
                else { cur.dir = DIR_UP_RIGHT; break;}
                if (cur.pos.x < BATTLE_W - 2) { (cur.pos.x) += 2; }
                else { cur.dir = DIR_DOWN_LEFT; break; }
                break;
            }
        }
    }
}

void check_user_status(int uid) {
    //log("checking...");
    //auto start_time = myclock();
    int bid = sessions[uid].bid;
    int ux = battles[bid].users[uid].pos.x;
    int uy = battles[bid].users[uid].pos.y;
    //for (int i = 0; i < MAX_ITEM; i++) {
    auto& items = battles[bid].items;
    if (battles[bid].users[uid].battle_state != BATTLE_STATE_LIVE) {
        return;
    }
    for (auto it = items.begin(), next = std::next(it); it != items.end(); it = next) {

        next = std::next(it);

        int ix = it->pos.x;
        int iy = it->pos.y;

        if (ix == ux && iy == uy) {
            switch (it->kind) {
                case ITEM_MAGAZINE: {
                    battles[bid].users[uid].energy += BULLETS_PER_MAGAZINE;
                    log("user #%d %s\033[2m(%s)\033[0m is got magazine", uid, sessions[uid].user_name, sessions[uid].ip_addr);
                    if (battles[bid].users[uid].energy > MAX_BULLETS) {
                        log("user #%d %s\033[2m(%s)\033[0m 's bullets exceeds max value", uid, sessions[uid].user_name, sessions[uid].ip_addr);
                        battles[bid].users[uid].energy = MAX_BULLETS;
                    }
                    send_to_client(uid, SERVER_MESSAGE_YOU_GOT_MAGAZINE);
                    it = items.erase(it);
                    //log("current item size: %ld", items.size());
                    break;
                }
                case ITEM_MAGMA: {
                    if (it->owner != uid) {
                        battles[bid].users[uid].life = max(battles[bid].users[uid].life - 1, 0);
                        battles[bid].users[uid].killby = it->owner;
                        it->count--;
                        log("user #%d %s\033[2m(%s)\033[0m is trapped in magma", uid, sessions[uid].user_name, sessions[uid].ip_addr);
                        send_to_client(uid, SERVER_MESSAGE_YOU_ARE_TRAPPED_IN_MAGMA);
                        if (it->count <= 0) {
                            log("magma #%d is exhausted", it->id);
                            battles[bid].num_of_other--;
                            it = items.erase(it);
                            //log("current item size: %ld", items.size());
                        }
                    }
                    break;
                }
                case ITEM_BLOOD_VIAL: {
                    battles[bid].users[uid].life += LIFE_PER_VIAL;
                    log("user #%d %s\033[2m(%s)\033[0m got blood vial", uid, sessions[uid].user_name, sessions[uid].ip_addr);
                    if (battles[bid].users[uid].life > MAX_LIFE) {
                        log("user #%d %s\033[2m(%s)\033[0m life exceeds max value", uid, sessions[uid].user_name, sessions[uid].ip_addr);
                        battles[bid].users[uid].life = MAX_LIFE;
                    }
                    //log("current item size: %ld", items.size());
                    battles[bid].num_of_other--;
                    send_to_client(uid, SERVER_MESSAGE_YOU_GOT_BLOOD_VIAL);
                    it = items.erase(it);
                    break;
                }
                case ITEM_BULLET: {
                    if (it->owner != uid) {
                        battles[bid].users[uid].life = max(battles[bid].users[uid].life - 1, 0);
                        battles[bid].users[uid].killby = it->owner;
                        log("user #%d %s\033[2m(%s)\033[0m is shooted", uid, sessions[uid].user_name, sessions[uid].ip_addr);
                        //log("current item size: %ld", items.size());
                        send_to_client(uid, SERVER_MESSAGE_YOU_ARE_SHOOTED);
                        it = items.erase(it);
                        break;
                    }
                    break;
                }
                case ITEM_LANDMINE: {
					if (it->owner != uid) {
						it->time = battles[bid].global_time;
						forced_generate_items(bid, ix, iy, ITEM_MAGMA, 7, it->owner);
						forced_generate_items(bid, ix - 1, iy, ITEM_MAGMA, 7, it->owner);
						forced_generate_items(bid, ix + 1, iy, ITEM_MAGMA, 7, it->owner);
						forced_generate_items(bid, ix, iy - 1, ITEM_MAGMA, 7, it->owner);
						forced_generate_items(bid, ix, iy + 1, ITEM_MAGMA, 7, it->owner);
					}
                    break;
                }
            }
        }
    }
    //auto end_time = myclock();
    //log("completed.");
}

void check_all_user_status(int bid) {
    //for (int i = 0; i < MAX_ITEM; i++) {
    //log("checking...");
    //log("completed.");
    for (int i = 0; i < USER_CNT; i++) {
        if (battles[bid].users[i].battle_state != BATTLE_STATE_LIVE) continue;
        check_user_status(i);
    }
}

void check_who_is_dead(int bid) {
    for (int i = 0; i < USER_CNT; i++) {
        if (battles[bid].users[i].battle_state == BATTLE_STATE_LIVE
            && battles[bid].users[i].life <= 0) {
            log("user #%d %s\033[2m(%s)\033[0m is dead", i, sessions[i].user_name, sessions[i].ip_addr);
            battles[bid].users[i].battle_state = BATTLE_STATE_DEAD;
            battles[bid].alive_users--;
            log("send dead info to user #%d %s\033[2m(%s)\033[0m", i, sessions[i].user_name, sessions[i].ip_addr);
            send_to_client(i, SERVER_MESSAGE_YOU_ARE_DEAD);
            sessions[i].death++;
            log("death of user #%d %s\033[2m(%s)\033[0m: %d", i, sessions[i].user_name, sessions[i].ip_addr, sessions[i].death);
            if (battles[bid].users[i].killby != -1) {
                int by = battles[bid].users[i].killby;
                sessions[by].kill++;
                log("kill of user #%d %s\033[2m(%s)\033[0m: %d", i, sessions[by].user_name, sessions[by].ip_addr, sessions[by].kill);
                double delta = (double)sessions[i].score / sessions[by].score;
                delta = delta * delta;
                if (delta > 4) delta = 4;
                if (delta < 0.2) delta = 0.2;
                int d = min(round(5. * delta), sessions[i].score);
                sessions[i].score -= d;
                sessions[by].score += d;
                battles[bid].users[by].energy += battles[bid].users[i].energy;
            } else {
                sessions[i].score = max(sessions[i].score - 5, 0);
            }
        } else if (battles[bid].users[i].battle_state == BATTLE_STATE_DEAD) {
            battles[bid].users[i].battle_state = BATTLE_STATE_WITNESS;
            battles[bid].users[i].energy = 0;
            battles[bid].users[i].life = 0;
        }
    }
}

void clear_items(int bid) {
    //log("call func %s", __func__);
    //for (int i = 0; i < MAX_ITEM; i++) {
    //    if (battles[bid].items[i].times) {
    //        battles[bid].items[i].times--;
    //        if (!battles[bid].items[i].times) {
    //            log("free item #%d", i);
    //            battles[bid].items[i].is_used = false;
    //            if (battles[bid].items[i].kind < ITEM_END) {
    //                battles[bid].num_of_other--;
    //            }
    //            //battles[bid].items[i].kind = ITEM_BLANK;
    //        }
    //    }
    //}
    //log("check completed...");
    auto& items = battles[bid].items;
    size_t cnt[ITEM_SIZE] = {0};
    for (auto cur = items.begin(), next = std::next(cur);
              cur != items.end();
              cur = next) {
        next = std::next(cur);
        if (cur->time <= battles[bid].global_time) {
            if (cur->kind < ITEM_END) {
                battles[bid].num_of_other--;
            }
            cnt[cur->kind]++;
            next = battles[bid].items.erase(cur);
        }
    }
    //int cleared = 0;
    for (int i = 0; i < ITEM_SIZE; i++) {
        if (cnt[i]) {
            log("clear %ld %s(s)", cnt[i], item_s[i]);
            //cleared = 1;
        }
    }
    //if (cleared) log("current item size: %ld", items.size());
}

void render_map_for_user(int uid, server_message_t* psm) {
    int bid = sessions[uid].bid;
    int map[BATTLE_H][BATTLE_W] = {0};
    int cur, x, y;
    //for (int i = 0, x, y; i < MAX_ITEM; i++) {
    for (auto it : battles[bid].items) {
        x = it.pos.x;
        y = it.pos.y;
        switch (it.kind) {
            case ITEM_BULLET: {
                if (it.owner == uid) {
                    map[y][x] = max(map[y][x], MAP_ITEM_MY_BULLET);
                } else {
                    map[y][x] = max(map[y][x], MAP_ITEM_OTHER_BULLET);
                }
                break;
            }
            case ITEM_LANDMINE: {
                if (it.owner != uid) break;
                map[y][x] = max(map[y][x], item_to_map[ITEM_LANDMINE]);
            }
            default: {
                cur = item_to_map[it.kind];
                map[y][x] = max(map[y][x], cur);
            }
        }
        //sm.item_kind[i] = it.kind;
        //sm.item_pos[i].x = it.pos.x;
        //sm.item_pos[i].y = it.pos.y;
    }
    for (int i = 0; i < BATTLE_H; i++) {
        for (int j = 0; j < BATTLE_W; j += 2) {
            //psm->map[i][j] = map[i][j];
            //if (psm->map[i][j] != 0) log("set item #%d", psm->map[i][j]);
            psm->map[i][j >> 1] = (map[i][j]) | (map[i][j + 1] << 4);
        }
    }
}

void inform_all_user_battle_player(int bid) {
    server_message_t sm;
    sm.message = SERVER_MESSAGE_BATTLE_PLAYER;
    for (int i = 0; i < USER_CNT; i++) {
        if (battles[bid].users[i].battle_state == BATTLE_STATE_LIVE &&
            battles[bid].users[i].life > 0) {
            strncpy(sm.users[i].name, sessions[i].user_name, USERNAME_SIZE - 1);
            sm.users[i].namecolor = i % color_s_size + 1;
            sm.users[i].life = battles[bid].users[i].life;
            sm.users[i].score = sessions[i].score;
            sm.users[i].death = sessions[i].death;
            sm.users[i].kill = sessions[i].kill;
        } else {
            strcpy(sm.users[i].name, (char*)"");
            sm.users[i].namecolor = 0;
            sm.users[i].life = 0;
            sm.users[i].score = 0;
            sm.users[i].death = 0;
            sm.users[i].kill = 0;
        }
    }
    for (int i = 0; i < USER_CNT; i++) {
        for (int j = i + 1; j < USER_CNT; j++) {
            if (sm.users[i].score < sm.users[j].score) {
                std::swap(sm.users[i], sm.users[j]);
            }
        }
    }
    for (int i = 0; i < USER_CNT; i++) {
        if (battles[bid].users[i].battle_state != BATTLE_STATE_UNJOINED) {
            wrap_send(sessions[i].conn, &sm);
            //log("inform user #%d %s\033[2m(%s)\033[0m", i, sessions[i].user_name, sessions[i].ip_addr);
        }
    }
}

void inform_all_user_battle_state(int bid) {
    server_message_t sm;
    sm.message = SERVER_MESSAGE_BATTLE_INFORMATION;
    for (int i = 0; i < USER_CNT; i++) {
        if (battles[bid].users[i].battle_state == BATTLE_STATE_LIVE) {
            sm.user_pos[i].x = battles[bid].users[i].pos.x;
            sm.user_pos[i].y = battles[bid].users[i].pos.y;
            sm.user_color[i] = i % color_s_size + 1;
        } else {
            sm.user_pos[i].x = -1;
            sm.user_pos[i].y = -1;
            sm.user_color[i] = 0;
        }
    }

    for (int i = 0; i < USER_CNT; i++) {
        if (battles[bid].users[i].battle_state != BATTLE_STATE_UNJOINED) {
            render_map_for_user(i, &sm);
            sm.index = i;
            sm.life = battles[bid].users[i].life;
            sm.bullets_num = battles[bid].users[i].energy;
            sm.color = i % color_s_size + 1;
            wrap_send(sessions[i].conn, &sm);
        }
    }
}

void* battle_ruler(void* args) {
    int bid = (int)(uintptr_t)args;
    log("battle ruler for battle #%d", bid);
    // FIXME: battle re-alloced before exiting loop 
    for (int i = 0; i < INIT_GRASS; i++) {
        forced_generate_items(bid,
                              (rand() & 0x7FFF) % BATTLE_W,
                              (rand() & 0x7FFF) % BATTLE_H,
                              ITEM_GRASS,
                              10000);
    }
    uint64_t  t[2];
    while (battles[bid].is_alloced) {
        battles[bid].global_time++;
        t[0] = myclock();
        move_bullets(bid);
        check_all_user_status(bid);
        check_who_is_dead(bid);
        inform_all_user_battle_state(bid);
        if (battles[bid].global_time % 10 == 0) {
            inform_all_user_battle_player(bid);
        }
        clear_items(bid);
        random_generate_items(bid);
        t[1] = myclock();
        if (t[1] - t[0] >= 5) logw("current delay %lums", t[1] - t[0]);
        //sum_delay_time += t[1] - t[0];
        while (myclock() < t[0] + GLOBAL_SPEED) usleep(1000);
    }
    return NULL;
}

int check_user_registered(char* user_name, char* password) {
    for (int i = 0; i < REGISTERED_USER_LIST_SIZE; i++) {
        if (strncmp(user_name, registered_user_list[i].user_name, USERNAME_SIZE - 1) != 0)
            continue;

        if (strncmp(password, registered_user_list[i].password, PASSWORD_SIZE - 1) != 0) {
            logi("user name %s sent error password", user_name);
            return SERVER_RESPONSE_LOGIN_FAIL_ERROR_PASSWORD;
        } else {
            return SERVER_RESPONSE_LOGIN_SUCCESS;
        }
    }

    logi("user name %s hasn't been registered", user_name);
    return SERVER_RESPONSE_LOGIN_FAIL_UNREGISTERED_USERID;
}

void launch_battle(int bid) {
    pthread_t thread;

    log("try to create battle_ruler thread");
    if (pthread_create(&thread, NULL, battle_ruler, (void*)(uintptr_t)bid) == -1) {
        eprintf("fail to launch battle");
    }
}

int client_command_user_register(int uid) {
    int ul_index = -1;
    char* user_name = sessions[uid].cm.user_name;
    char* password = sessions[uid].cm.password;
    log("user %s tries to register with password %s", user_name, password);

    for (int i = 0; i < REGISTERED_USER_LIST_SIZE; i++) {
        if (strncmp(user_name, registered_user_list[i].user_name, USERNAME_SIZE - 1) != 0)
            continue;

        log("user %s&%s has been registered", user_name, password);
        send_to_client(uid, SERVER_RESPONSE_YOU_HAVE_REGISTERED);
        return 0;
    }

    pthread_mutex_lock(&userlist_lock);
    if (user_list_size < REGISTERED_USER_LIST_SIZE)
        ul_index = user_list_size++;
    pthread_mutex_unlock(&userlist_lock);

    log("fetch empty user list index #%d", ul_index);
    if (ul_index == -1) {
        log("user %s registers fail", user_name);
        send_to_client(uid, SERVER_RESPONSE_REGISTER_FAIL);
    } else {
        log("user %s registers success", user_name);
        strncpy(registered_user_list[ul_index].user_name,
                user_name, USERNAME_SIZE - 1);
        strncpy(registered_user_list[ul_index].password,
                password, PASSWORD_SIZE - 1);
        send_to_client(uid, SERVER_RESPONSE_REGISTER_SUCCESS);
        save_user(ul_index);
    }
    return 0;
}

int client_command_user_login(int uid) {
    int is_dup = 0;
    client_message_t* pcm = &sessions[uid].cm;
    char* user_name = pcm->user_name;
    char* password = pcm->password;
    char* ip_addr = sessions[uid].ip_addr;
    log("user #%d %s\033[2m(%s)\033[0m try to login", uid, user_name, ip_addr);
    int message = check_user_registered(user_name, password);

    if (query_session_built(uid)) {
        log("user #%d %s\033[2m(%s)\033[0m has logined", uid, sessions[uid].user_name, sessions[uid].ip_addr);
        send_to_client(uid, SERVER_RESPONSE_YOU_HAVE_LOGINED);
        return 0;
    }

    for (int i = 0; i < USER_CNT; i++) {
        if (query_session_built(i)) {
            logi("check dup user id: %s vs. %s", user_name, sessions[i].user_name);
            if (strncmp(user_name, sessions[i].user_name, USERNAME_SIZE - 1) == 0) {
                log("user #%d %s duplicate with %dth user %s\033[2m(%s)\033[0m", uid, user_name, i, sessions[i].user_name, sessions[i].ip_addr);
                is_dup = 1;
                break;
            }
        }
    }

    // no duplicate user ids found
    if (is_dup) {
        log("send fail dup id message to client.");
        send_to_client(uid, SERVER_RESPONSE_LOGIN_FAIL_DUP_USERID);
        sessions[uid].state = USER_STATE_NOT_LOGIN;
    } else if (message == SERVER_RESPONSE_LOGIN_SUCCESS) {
        log("user %s login success", user_name);
        sessions[uid].state = USER_STATE_LOGIN;
        send_to_client(
            uid,
            SERVER_RESPONSE_LOGIN_SUCCESS,
            sformat("Welcome to multiplayer shooting game! server \033[0;32m%s%s", version, color_s[0]));
        strncpy(sessions[uid].user_name, user_name, USERNAME_SIZE - 1);
        inform_friends(uid, SERVER_MESSAGE_FRIEND_LOGIN);
    } else {
        send_to_client(uid, message);
    }

    return 0;
}

int client_command_user_logout(int uid) {
    if (sessions[uid].state == USER_STATE_BATTLE
        || sessions[uid].state == USER_STATE_WAIT_TO_BATTLE) {
        log("user #%d %s\033[2m(%s)\033[0m tries to logout was in battle", uid, sessions[uid].user_name, sessions[uid].ip_addr);
        user_quit_battle(sessions[uid].bid, uid);
    }

    log("user #%d %s\033[2m(%s)\033[0m logout", uid, sessions[uid].user_name, sessions[uid].ip_addr);
    sessions[uid].state = USER_STATE_NOT_LOGIN;
    inform_friends(uid, SERVER_MESSAGE_FRIEND_LOGOUT);
    return 0;
}

void list_all_users(server_message_t* psm) {
    for (int i = 0; i < USER_CNT; i++) {
        if (query_session_built(i)) {
            log("%s: found %s %s", __func__, sessions[i].user_name,
                sessions[i].state == USER_STATE_BATTLE ? "in battle" : "");
            psm->all_users[i].user_state = sessions[i].state;
            strncpy(psm->all_users[i].user_name, sessions[i].user_name, USERNAME_SIZE - 1);
        }
    }
}

int client_command_fetch_all_users(int uid) {
    char* user_name = sessions[uid].user_name;
    log("user #%d %s\033[2m(%s)\033[0m tries to fetch all users's info", uid, user_name, sessions[uid].user_name);

    if (!query_session_built(uid)) {
        logi("user #%d %s\033[2m(%s)\033[0m who tries to list users hasn't login", uid, user_name, sessions[uid].user_name);
        send_to_client(uid, SERVER_RESPONSE_YOU_HAVE_NOT_LOGIN);
        return 0;
    }

    server_message_t sm;
    memset(&sm, 0, sizeof(server_message_t));
    list_all_users(&sm);
    sm.response = SERVER_RESPONSE_ALL_USERS_INFO;

    wrap_send(sessions[uid].conn, &sm);

    return 0;
}

int client_command_fetch_all_friends(int uid) {
    char *user_name = sessions[uid].user_name;
    log("user %s tries to fetch info", user_name);

    if (!query_session_built(uid)) {
        logi("user %s who tries to list users hasn't login", user_name);
        send_to_client(uid, SERVER_RESPONSE_YOU_HAVE_NOT_LOGIN);
        return 0;
    }

    server_message_t sm;
    memset(&sm, 0, sizeof(server_message_t));
    list_all_users(&sm);
    sm.all_users[uid].user_state = USER_STATE_UNUSED;
    sm.response = SERVER_RESPONSE_ALL_FRIENDS_INFO;

    wrap_send(sessions[uid].conn, &sm);

    return 0;
}

int invite_friend_to_battle(int bid, int uid, char* friend_name) {
    int friend_id = find_uid_by_user_name(friend_name);
    if (friend_id == -1) {
        // fail to find friend
        logi("friend %s hasn't login", friend_name);
        send_to_client(uid, SERVER_MESSAGE_FRIEND_NOT_LOGIN);
    } else if (friend_id == uid) {
        logi("launch battle %d for %s", bid, sessions[uid].user_name);
        sessions[uid].inviter_id = uid;
        send_to_client(uid, SERVER_RESPONSE_INVITATION_SENT);
    } else if (sessions[friend_id].state == USER_STATE_BATTLE) {
        // friend already in battle
        logi("friend %s already in battle", friend_name);
        send_to_client(uid, SERVER_MESSAGE_FRIEND_ALREADY_IN_BATTLE);
    } else {
        // invite friend
        logi("friend #%d %s found", friend_id, friend_name);

        user_invited_to_join_battle(bid, friend_id);
        // WARNING: can't move this statement
        sessions[friend_id].inviter_id = uid;

        send_to_client_with_username(friend_id, SERVER_MESSAGE_INVITE_TO_BATTLE, sessions[uid].user_name);
    }

    return 0;
}

int client_command_launch_battle(int uid) {
    if (sessions[uid].state == USER_STATE_BATTLE) {
        log("user %s who tries to launch battle has been in battle", sessions[uid].user_name);
        send_to_client(uid, SERVER_RESPONSE_YOURE_ALREADY_IN_BATTLE);
        return 0;
    } else {
        log("user %s tries to launch battle", sessions[uid].user_name);
    }

    int bid = get_unalloced_battle();
    client_message_t* pcm = &sessions[uid].cm;

    log("%s launch battle with %s", sessions[uid].user_name, pcm->user_name);

    if (bid == -1) {
        loge("fail to create battle for %s and %s", sessions[uid].user_name, pcm->user_name);
        send_to_client(uid, SERVER_RESPONSE_LAUNCH_BATTLE_FAIL);
        return 0;
    } else {
        logi("launch battle %d for %s, invite %s", bid, sessions[uid].user_name, pcm->user_name);
        user_join_battle(bid, uid);
        if (strcmp(pcm->user_name, ""))
            invite_friend_to_battle(bid, uid, pcm->user_name);
        launch_battle(bid);
        send_to_client(uid, SERVER_RESPONSE_LAUNCH_BATTLE_SUCCESS);
    }

    return 0;
}

int client_command_quit_battle(int uid) {
    log("user %s tries to quit battle", sessions[uid].user_name);
    if (sessions[uid].state != USER_STATE_BATTLE) {
        logi("but he hasn't join battle");
        send_to_client(uid, SERVER_RESPONSE_YOURE_NOT_IN_BATTLE);
    } else {
        logi("call user_quit_battle to quit");
        user_quit_battle(sessions[uid].bid, uid);
    }
    return 0;
}

int client_command_invite_user(int uid) {
    client_message_t* pcm = &sessions[uid].cm;
    int bid = sessions[uid].bid;
    int friend_id = find_uid_by_user_name(pcm->user_name);
    log("user #%d %s\033[2m(%s)\033[0m tries to invite friend", uid, sessions[uid].user_name, sessions[uid].ip_addr);

    if (sessions[uid].state != USER_STATE_BATTLE) {
        log("user %s\033[2m(%s)\033[0m who invites friend %s wasn't in battle", sessions[uid].user_name, sessions[uid].ip_addr, pcm->user_name);
        send_to_client(uid, SERVER_RESPONSE_YOURE_NOT_IN_BATTLE);
    } else {
        logi("invite user %s\033[2m(%s)\033[0m to battle #%d", sessions[friend_id].user_name, sessions[uid].ip_addr, bid);
        invite_friend_to_battle(bid, uid, pcm->user_name);
    }
    return 0;
}

int client_command_send_message(int uid) {
    client_message_t* pcm = &sessions[uid].cm;
    server_message_t sm;
    memset(&sm, 0, sizeof(server_message_t));
    sm.message = SERVER_MESSAGE_FRIEND_MESSAGE;
    strncpy(sm.from_user, sessions[uid].user_name, USERNAME_SIZE);
    strncpy(sm.msg, pcm->message, MSG_SIZE);
    if (pcm->user_name[0] == '\0') {
        logi("user %d:%s\033[2m(%s)\033[0m yells at all users: %s", uid, sessions[uid].user_name, sessions[uid].ip_addr, pcm->message);
        int i;
        for (i = 0; i < USER_CNT; i++) {
            if (uid == i) continue;
            wrap_send(sessions[i].conn, &sm);
        }
    } else {
        int friend_id = find_uid_by_user_name(pcm->user_name);
        if (friend_id == -1 || friend_id == uid) {
            logi("user %d:%s\033[2m(%s)\033[0m fails to speak to %s:`%s`", uid, sessions[uid].user_name, sessions[uid].ip_addr, pcm->user_name, pcm->message);
        } else {
            logi("user %d:%s\033[2m(%s)\033[0m speaks to %d:%s : `%s`", uid, sessions[uid].user_name, sessions[uid].ip_addr, friend_id, pcm->user_name, pcm->message);
            wrap_send(sessions[friend_id].conn, &sm);
        }
    }
    return 0;
}

int client_command_create_ffa(int uid) {
    if (sessions[uid].state == USER_STATE_BATTLE) {
        log("user %s who tries to launch battle has been in battle", sessions[uid].user_name);
        send_to_client(uid, SERVER_RESPONSE_YOURE_ALREADY_IN_BATTLE);
        return 0;
    } else {
        log("user %s tries to create ffa sessions #0", sessions[uid].user_name);
    }

    int bid = 0;
    client_message_t* pcm = &sessions[uid].cm;

    log("%s launch battle with %s", sessions[uid].user_name, pcm->user_name);

    if (battles[bid].is_alloced) {
        loge("fail to create battle for %s and %s", sessions[uid].user_name, pcm->user_name);
        send_to_client(uid, SERVER_RESPONSE_LAUNCH_BATTLE_FAIL);
        return 0;
    } else {
        logi("launch battle #0 for ffa");
        battles[bid].is_alloced = true;
        user_join_battle(bid, uid);
        if (strcmp(pcm->user_name, ""))
            invite_friend_to_battle(bid, uid, pcm->user_name);
        launch_battle(bid);
        send_to_client(uid, SERVER_RESPONSE_LAUNCH_BATTLE_SUCCESS);
    }

    return 0;
}

int client_command_launch_ffa(int uid) {
    log("user %s\033[2m(%s)\033[0m try ffa", sessions[uid].user_name, sessions[uid].ip_addr);

    if (sessions[uid].state == USER_STATE_BATTLE) {
        logi("already in battle");
        send_to_client(uid, SERVER_RESPONSE_YOURE_ALREADY_IN_BATTLE);
    } else {
        int bid = 0;

        if (battles[bid].is_alloced) {
            user_join_battle(bid, uid);
            logi("accept success");
        } else {
            logi("user %s created ffa session #0", sessions[uid].user_name);
            client_command_create_ffa(uid);
        }
    }
    return 0;
}

int client_command_accept_battle(int uid) {
    log("user %s\033[2m(%s)\033[0m accept battle #%d", sessions[uid].user_name, sessions[uid].ip_addr, sessions[uid].bid);

    if (sessions[uid].state == USER_STATE_BATTLE) {
        logi("already in battle");
        send_to_client(uid, SERVER_RESPONSE_YOURE_ALREADY_IN_BATTLE);
    } else if (sessions[uid].state == USER_STATE_WAIT_TO_BATTLE) {
        int inviter_id = sessions[uid].inviter_id;
        int bid = sessions[uid].bid;

        if (battles[bid].is_alloced) {
            send_to_client_with_username(inviter_id, SERVER_MESSAGE_FRIEND_ACCEPT_BATTLE, sessions[inviter_id].user_name);
            user_join_battle(bid, uid);
            logi("accept success");
        } else {
            logi("user %s\033[2m(%s)\033[0m accept battle which didn't exist", sessions[uid].user_name, sessions[uid].ip_addr);
            send_to_client(uid, SERVER_RESPONSE_YOURE_ALREADY_IN_BATTLE);
        }

    } else {
        logi("hasn't been invited");
        send_to_client(uid, SERVER_RESPONSE_NOBODY_INVITE_YOU);
    }

    return 0;
}

int client_command_reject_battle(int uid) {
    log("user %s\033[2m(%s)\033[0m reject battle #%d", sessions[uid].user_name, sessions[uid].ip_addr, sessions[uid].bid);
    if (sessions[uid].state == USER_STATE_BATTLE) {
        logi("user already in battle");
        send_to_client(uid, SERVER_RESPONSE_YOURE_ALREADY_IN_BATTLE);
    } else if (sessions[uid].state == USER_STATE_WAIT_TO_BATTLE) {
        logi("reject success");
        int bid = sessions[uid].bid;
        send_to_client(sessions[uid].inviter_id, SERVER_MESSAGE_FRIEND_REJECT_BATTLE);
        sessions[uid].state = USER_STATE_LOGIN;
        battles[bid].users[uid].battle_state = BATTLE_STATE_UNJOINED;
    } else {
        logi("hasn't been invited");
        send_to_client(uid, SERVER_RESPONSE_NOBODY_INVITE_YOU);
    }
    return 0;
}

int client_command_quit(int uid) {
    int conn = sessions[uid].conn;
    if (sessions[uid].state == USER_STATE_BATTLE
        || sessions[uid].state == USER_STATE_WAIT_TO_BATTLE) {
        log("user #%d %s tries to quit client was in battle", uid, sessions[uid].user_name);
        user_quit_battle(sessions[uid].bid, uid);
    }

    if (sessions[uid].conn >= 0) {
        sessions[uid].conn = -1;
        log("user #%d %s quit", uid, sessions[uid].user_name);
        sessions[uid].state = USER_STATE_UNUSED;
        close(conn);
    }
    return -1;
}

int client_command_move_up(int uid) {
    log("user #%d %s\033[2m(%s)\033[0m move up", uid, sessions[uid].user_name, sessions[uid].ip_addr);
    int bid = sessions[uid].bid;
    battles[bid].users[uid].dir = DIR_UP;
    if (battles[bid].users[uid].pos.y > 0) {
        battles[bid].users[uid].pos.y--;
        check_user_status(uid);
    }
    return 0;
}

int client_command_move_down(int uid) {
    log("user #%d %s\033[2m(%s)\033[0m move down", uid, sessions[uid].user_name, sessions[uid].ip_addr);
    int bid = sessions[uid].bid;
    battles[bid].users[uid].dir = DIR_DOWN;
    if (battles[bid].users[uid].pos.y < BATTLE_H - 1) {
        battles[bid].users[uid].pos.y++;
        check_user_status(uid);
    }
    return 0;
}

int client_command_move_left(int uid) {
    log("user #%d %s\033[2m(%s)\033[0m move left", uid, sessions[uid].user_name, sessions[uid].ip_addr);
    int bid = sessions[uid].bid;
    battles[bid].users[uid].dir = DIR_LEFT;
    if (battles[bid].users[uid].pos.x > 0) {
        battles[bid].users[uid].pos.x--;
        check_user_status(uid);
    }
    return 0;
}

int client_command_move_right(int uid) {
    log("user #%d %s\033[2m(%s)\033[0m move right", uid, sessions[uid].user_name, sessions[uid].ip_addr);
    int bid = sessions[uid].bid;
    battles[bid].users[uid].dir = DIR_RIGHT;
    if (battles[bid].users[uid].pos.x < BATTLE_W - 1) {
        battles[bid].users[uid].pos.x++;
        check_user_status(uid);
    }
    return 0;
}

int client_command_put_landmine(int uid) {
    int bid = sessions[uid].bid;

    if (battles[bid].users[uid].energy < LANDMINE_COST) {
        send_to_client(uid, SERVER_MESSAGE_YOUR_MAGAZINE_IS_EMPTY);
        return 0;
    }
    int x = battles[bid].users[uid].pos.x;
    int y = battles[bid].users[uid].pos.y;
    if (x < 0 || x >= BATTLE_W) return 1;
    if (y < 0 || y >= BATTLE_H) return 1;
    log("user #%d %s\033[2m(%s)\033[0m put at (%d, %d)", uid, sessions[uid].user_name, sessions[uid].ip_addr, x, y);
    item_t new_item;
    new_item.id = ++battles[bid].item_count;
    new_item.kind = ITEM_LANDMINE;
    new_item.owner = uid;
    new_item.pos.x = x;
    new_item.pos.y = y;
    new_item.time = battles[bid].global_time + INF;
    battles[bid].users[uid].energy -= LANDMINE_COST;
    battles[bid].items.push_back(new_item);
    //log("current item size: %ld", battles[bid].items.size());
    return 0;
}

int client_command_fire(int uid, int delta_x, int delta_y, int dir) {
    int bid = sessions[uid].bid;

    if (battles[bid].users[uid].energy <= 0) {
        send_to_client(uid, SERVER_MESSAGE_YOUR_MAGAZINE_IS_EMPTY);
        return 0;
    }
    int x = battles[bid].users[uid].pos.x + delta_x;
    int y = battles[bid].users[uid].pos.y + delta_y;
    if (x < 0 || x >= BATTLE_W) return 1;
    if (y < 0 || y >= BATTLE_H) return 1;
    log("user #%d %s\033[2m(%s)\033[0m fire %s", uid, sessions[uid].user_name, sessions[uid].ip_addr, dir_s[dir]);
    item_t new_item;
    new_item.id = ++battles[bid].item_count;
    new_item.kind = ITEM_BULLET;
    new_item.dir = dir;
    new_item.owner = uid;
    new_item.pos.x = x;
    new_item.pos.y = y;
    new_item.time = battles[bid].global_time + BULLETS_LASTS_TIME;
    battles[bid].users[uid].energy--;
    battles[bid].items.push_back(new_item);
    //log("current item size: %ld", battles[bid].items.size());
    return 0;
}

int client_command_fire_up(int uid) {
    logi("client_command_fire");
    client_command_fire(uid, 0, 0, DIR_UP);
    return 0;
}
int client_command_fire_down(int uid) {
    logi("client_command_fire");
    client_command_fire(uid, 0, 0, DIR_DOWN);
    return 0;
}
int client_command_fire_left(int uid) {
    logi("client_command_fire");
    client_command_fire(uid, 0, 0, DIR_LEFT);
    return 0;
}
int client_command_fire_right(int uid) {
    logi("client_command_fire");
    client_command_fire(uid, 0, 0, DIR_RIGHT);
    return 0;
}

int client_command_fire_up_left(int uid) {
    logi("client_command_fire");
    client_command_fire(uid, 0, 0, DIR_UP_LEFT);
    return 0;
}
int client_command_fire_up_right(int uid) {
    logi("client_command_fire");
    client_command_fire(uid, 0, 0, DIR_UP_RIGHT);
    return 0;
}
int client_command_fire_down_left(int uid) {
    logi("client_command_fire");
    client_command_fire(uid, 0, 0, DIR_DOWN_LEFT);
    return 0;
}
int client_command_fire_down_right(int uid) {
    logi("client_command_fire");
    client_command_fire(uid, 0, 0, DIR_DOWN_RIGHT);
    return 0;
}

int client_command_fire_aoe(int uid, int dir) {
    log("user #%d %s\033[2m(%s)\033[0m fire(aoe) %s", uid, sessions[uid].user_name, sessions[uid].ip_addr, dir_s[dir]);
    logi("call client_command_fire");
    int limit = battles[sessions[uid].bid].users[uid].energy / 2, cnt = 0;
    for (int i = 0; limit; i++) {
        for (int j = -i; j <= i && limit; j++) {
            switch (dir) {
                case DIR_UP: {
                    if (client_command_fire(uid, j, -i + abs(j), dir) == 0) cnt++;
                    break;
                }
                case DIR_DOWN: {
                    if (client_command_fire(uid, j, i - abs(j), dir) == 0) cnt++;
                    break;
                }
                case DIR_LEFT: {
                    if (client_command_fire(uid, -i + abs(j), j, dir) == 0) cnt++;
                    break;
                }
                case DIR_RIGHT: {
                    if (client_command_fire(uid, i - abs(j), j, dir) == 0) cnt++;
                    break;
                }
            }
            limit--;
        }
    }
    log("created %d bullets", cnt);
    return 0;
}

int client_command_fire_aoe_up(int uid) {
    logi("call client_command_fire_aoe");
    return client_command_fire_aoe(uid, DIR_UP);
}
int client_command_fire_aoe_down(int uid) {
    logi("call client_command_fire_aoe");
    return client_command_fire_aoe(uid, DIR_DOWN);
}
int client_command_fire_aoe_left(int uid) {
    logi("call client_command_fire_aoe");
    return client_command_fire_aoe(uid, DIR_LEFT);
}
int client_command_fire_aoe_right(int uid) {
    logi("call client_command_fire_aoe");
    return client_command_fire_aoe(uid, DIR_RIGHT);
}

int client_command_melee(int uid) {
    int bid = sessions[uid].bid;
    if (battles[bid].users[uid].life <= 0) return 0;
    int dir = battles[bid].users[uid].dir;
    int x = battles[bid].users[uid].pos.x;
    int y = battles[bid].users[uid].pos.y;
    log("user #%d %s\033[2m(%s)\033[0m melee %s", uid, sessions[uid].user_name, sessions[uid].ip_addr, dir_s[dir]);
    for (int i = 1; i <= 3; i++) {
        forced_generate_items(bid, 
                              x + dir_offset[dir].x * i,
                              y + dir_offset[dir].y * i,
                              ITEM_MAGMA,
                              3,
                              uid);
    }
    return 0;
}

int admin_set_admin(int argc, char** argv) {
    if (argc < 3) return -1;
    int uid = find_uid_by_user_name(argv[1]), status = atoi(argv[2]);
    if (uid < 0 || uid >= USER_CNT || sessions[uid].conn < 0) {
        return -1;
    }
    if (status) log("admin set user #%d admin", uid);
    else log("admin set user #%d non-admin", uid);
    sessions[uid].is_admin = status;
    for (int i = 0; i < USER_CNT; i++) {
        if (sessions[i].conn >= 0) {
            if (status) {
                say_to_client(i, sformat("admin set user #%d %s to admin", uid, sessions[uid].user_name));
            } else {
                say_to_client(i, sformat("admin set user #%d %s to non-admin", uid, sessions[uid].user_name));
            }
        }
    }
    return 0;
}

int admin_set_energy(int argc, char** argv) {
    if (argc < 3) return -1;
    int uid = find_uid_by_user_name(argv[1]), energy = atoi(argv[2]);
    log("admin set user #%d's energy", uid);
    if (uid < 0 || uid >= USER_CNT || sessions[uid].conn < 0 || energy < 0) {
        return -1;
    }
    log("admin set user #%d %s's energy to %d", uid, sessions[uid].user_name, energy);
    battles[sessions[uid].bid].users[uid].energy = energy;
    for (int i = 0; i < USER_CNT; i++) {
        if (sessions[i].conn >= 0) {
            say_to_client(i, sformat("admin set user #%d %s's energy to %d", uid, sessions[uid].user_name, energy));
        }
    }
    return 0;
}

int admin_set_hp(int argc, char** argv) {
    if (argc < 3) return -1;
    int uid = find_uid_by_user_name(argv[1]), hp = atoi(argv[2]);
    if (uid < 0 || uid >= USER_CNT || sessions[uid].conn < 0 || hp < 0) {
        return -1;
    }
    log("admin set user #%d %s's hp to %d", uid, sessions[uid].user_name, hp);
    battles[sessions[uid].bid].users[uid].life = hp;
    for (int i = 0; i < USER_CNT; i++) {
        if (sessions[i].conn >= 0) {
            say_to_client(i, sformat("admin set user #%d %s's hp to %d", uid, sessions[uid].user_name, hp));
        }
    }
    return 0;
}

int admin_set_pos(int argc, char** argv) {
    if (argc < 4) return -1;
    int uid = find_uid_by_user_name(argv[1]);
    uint8_t x = atoi(argv[2]), y = atoi(argv[3]);
    if (uid < 0 || uid >= USER_CNT || sessions[uid].conn < 0) {
        return -1;
    }
    if (x < 0 || x >= BATTLE_W) return -1;
    if (y < 0 || y >= BATTLE_H) return -1;
    log("admin set user #%d %s's pos to (%d, %d)", uid, sessions[uid].user_name, x, y);
    battles[sessions[uid].bid].users[uid].pos.x = x;
    battles[sessions[uid].bid].users[uid].pos.y = y;
    return 0;
}

int admin_ban_user(int argc, char** argv) {
    if (argc < 2) return -1;
    int uid = find_uid_by_user_name(argv[1]);
    log("admin ban user #%d", uid);
    if (uid < 0 || uid >= USER_CNT) {
        logi("fail");
        return -1;
    }
    if (sessions[uid].conn >= 0) {
        log("admin banned user #%d %s\033[2m(%s)\033[0m", uid, sessions[uid].user_name, sessions[uid].ip_addr);
        send_to_client(
            uid, SERVER_STATUS_QUIT,
            (char*)" (you were banned by admin)");
        client_command_quit(uid);
        for (int i = 0; i < USER_CNT; i++) {
            if (sessions[i].conn >= 0) {
                say_to_client(i, sformat("admin banned user #%d %s\033[2m(%s)\033[0m", uid, sessions[uid].user_name, sessions[uid].ip_addr));
            }
        }
    }
    return 0;
}

static struct {
    const char* cmd;
    int (*func)(int argc, char** argv);
} admin_handler[] = {
    {"ban", admin_ban_user},
    {"eng", admin_set_energy},
    {"energy", admin_set_energy},
    {"hp", admin_set_hp},
    {"setadmin", admin_set_admin},
    {"pos", admin_set_pos},
};

#define NR_HANDLER ((int)sizeof(admin_handler) / (int)sizeof(admin_handler[0]))

int client_command_admin_control(int uid) {
    if (!sessions[uid].is_admin) {
        say_to_client(uid, (char*)"you are not admin");
        return 0;
    }
    client_message_t* pcm = &sessions[uid].cm;
    char *buff = (char*)pcm->message;
    log("analysis command `%s`", buff);
    char *go = buff, *argv[ADMIN_COMMAND_LEN];
    int argc = 0;
    while (*go != 0) {
        if (!isspace(*go)) {
            argv[argc] = go;
            argc++;
            char c = (*go == '"') ? '"' : ' ';
            while ((*go != 0) && (*go != c)) go++;
            if (*go == 0) break;
            *go = 0;
        }
        go++;
    }
    if (argc) {
        argv[argc] = NULL;
        for (int i = 0; i < NR_HANDLER; i++) {
            if (strcmp(argv[0], admin_handler[i].cmd) == 0) {
                if (admin_handler[i].func(argc, argv)) {
                    say_to_client(uid, (char*)"invalid command!");
                }
                return 0;
            }
        }
    }
    say_to_client(uid, (char*)"invalid command!");
    return 0;
}

int client_message_fatal(int uid) {
    loge("received FATAL from user #%d %s\033[2m(%s)\033[0m ", uid, sessions[uid].user_name, sessions[uid].ip_addr);
    for (int i = 0; i < USER_CNT; i++) {
        if (sessions[i].conn >= 0) {
            send_to_client(i, SERVER_STATUS_FATAL);
            log("send FATAL to user #%d %s\033[2m(%s)\033[0m", i, sessions[i].user_name, sessions[i].ip_addr);
        }
    }
    terminate_process(0);
    return 0;
}

static int (*handler[256])(int);

void init_handler() {
    handler[CLIENT_MESSAGE_FATAL] = client_message_fatal,

    handler[CLIENT_COMMAND_USER_QUIT] = client_command_quit,
    handler[CLIENT_COMMAND_USER_REGISTER] = client_command_user_register,
    handler[CLIENT_COMMAND_USER_LOGIN] = client_command_user_login,
    handler[CLIENT_COMMAND_USER_LOGOUT] = client_command_user_logout,

    handler[CLIENT_COMMAND_FETCH_ALL_USERS] = client_command_fetch_all_users,
    handler[CLIENT_COMMAND_FETCH_ALL_FRIENDS] = client_command_fetch_all_friends,

    handler[CLIENT_COMMAND_LAUNCH_BATTLE] = client_command_launch_battle,
    handler[CLIENT_COMMAND_QUIT_BATTLE] = client_command_quit_battle,
    handler[CLIENT_COMMAND_ACCEPT_BATTLE] = client_command_accept_battle,
    handler[CLIENT_COMMAND_LAUNCH_FFA] = client_command_launch_ffa,
    handler[CLIENT_COMMAND_REJECT_BATTLE] = client_command_reject_battle,
    handler[CLIENT_COMMAND_INVITE_USER] = client_command_invite_user,

    handler[CLIENT_COMMAND_SEND_MESSAGE] = client_command_send_message,

    handler[CLIENT_COMMAND_MOVE_UP] = client_command_move_up,
    handler[CLIENT_COMMAND_MOVE_DOWN] = client_command_move_down,
    handler[CLIENT_COMMAND_MOVE_LEFT] = client_command_move_left,
    handler[CLIENT_COMMAND_MOVE_RIGHT] = client_command_move_right,

    handler[CLIENT_COMMAND_PUT_LANDMINE] = client_command_put_landmine,
    
    handler[CLIENT_COMMAND_MELEE] = client_command_melee,

    handler[CLIENT_COMMAND_FIRE_UP] = client_command_fire_up,
    handler[CLIENT_COMMAND_FIRE_DOWN] = client_command_fire_down,
    handler[CLIENT_COMMAND_FIRE_LEFT] = client_command_fire_left,
    handler[CLIENT_COMMAND_FIRE_RIGHT] = client_command_fire_right,

    handler[CLIENT_COMMAND_FIRE_UP_LEFT] = client_command_fire_up_left,
    handler[CLIENT_COMMAND_FIRE_UP_RIGHT] = client_command_fire_up_right,
    handler[CLIENT_COMMAND_FIRE_DOWN_LEFT] = client_command_fire_down_left,
    handler[CLIENT_COMMAND_FIRE_DOWN_RIGHT] = client_command_fire_down_right,

    handler[CLIENT_COMMAND_FIRE_AOE_UP] = client_command_fire_aoe_up,
    handler[CLIENT_COMMAND_FIRE_AOE_DOWN] = client_command_fire_aoe_down,
    handler[CLIENT_COMMAND_FIRE_AOE_LEFT] = client_command_fire_aoe_left,
    handler[CLIENT_COMMAND_FIRE_AOE_RIGHT] = client_command_fire_aoe_right;

    handler[CLIENT_COMMAND_ADMIN_CONTROL] = client_command_admin_control;

}

void wrap_recv(int conn, client_message_t* pcm) {
    size_t total_len = 0;
    while (total_len < sizeof(client_message_t)) {
        size_t len = recv(conn, pcm + total_len, sizeof(client_message_t) - total_len, 0);
        if (len < 0) {
            loge("broken pipe");
        }

        total_len += len;
    }
}

void wrap_send(int conn, server_message_t* psm) {
    size_t total_len = 0;
    while (total_len < sizeof(server_message_t)) {
        size_t len = send(conn, psm + total_len, sizeof(server_message_t) - total_len, 0);
        if (len < 0) {
            loge("broken pipe");
        }

        total_len += len;
    }
}

void send_to_client(int uid, int message) {
    int conn = sessions[uid].conn;
    if (conn < 0) return;
    server_message_t sm;
    memset(&sm, 0, sizeof(server_message_t));
    sm.response = message;
    wrap_send(conn, &sm);
}

void send_to_client(int uid, int message, char* str) {
    int conn = sessions[uid].conn;
    if (conn < 0) return;
    server_message_t sm;
    memset(&sm, 0, sizeof(server_message_t));
    sm.response = message;
    strncpy(sm.msg, str, MSG_SIZE - 1);
    wrap_send(conn, &sm);
}

void say_to_client(int uid, char *message) {
    log("say `%s` to user #%d %s", message, uid, sessions[uid].user_name);
    int conn = sessions[uid].conn;
    if (conn < 0) { logi("fail"); return; }
    server_message_t sm;
    memset(&sm, 0, sizeof(server_message_t));
    sm.message = SERVER_MESSAGE;
    strncpy(sm.msg, message, MSG_SIZE - 1);
    wrap_send(conn, &sm);
}

void send_to_client_with_username(int uid, int message, char* user_name) {
    int conn = sessions[uid].conn;
    if (conn < 0) return;
    server_message_t sm;
    memset(&sm, 0, sizeof(server_message_t));
    sm.response = message;
    strncpy(sm.friend_name, user_name, USERNAME_SIZE - 1);
    wrap_send(conn, &sm);
}

void close_session(int conn, int message) {
    send_to_client(conn, message);
    close(conn);
}

void* session_start(void* args) {
    int uid = -1;
    session_args_t info = *(session_args_t*)(uintptr_t)args;
    client_message_t* pcm = NULL;
    if ((uid = get_unused_session()) < 0) {
        close_session(info.conn, SERVER_RESPONSE_LOGIN_FAIL_SERVER_LIMITS);
        return NULL;
    } else {
        sessions[uid].conn = info.conn;
        strncpy(sessions[uid].user_name, "<unknown>", USERNAME_SIZE - 1);
        strncpy(sessions[uid].ip_addr, info.ip_addr, IPADDR_SIZE - 1);
        if (strncmp(sessions[uid].ip_addr, "", IPADDR_SIZE) == 0) {
            strncpy(sessions[uid].ip_addr, "unknown", IPADDR_SIZE - 1);
        }
        pcm = &sessions[uid].cm;
        memset(pcm, 0, sizeof(client_message_t));
        log("build session #%d", uid);
        if (strncmp(info.ip_addr, "127.0.0.1", IPADDR_SIZE) == 0) {
            log("admin login!");
            sessions[uid].is_admin = 1;
        }
        sessions[uid].death = sessions[uid].kill = 0;
        sessions[uid].score = 50;
    }

    while (1) {
        wrap_recv(info.conn, pcm);
        if (pcm->command >= CLIENT_COMMAND_END)
            continue;

        int ret_code = handler[pcm->command](uid);
        if (ret_code < 0) {
            log("close session #%d", uid);
            break;
        }
    }
    return NULL;
}

void* run_battle(void* args) {
    // TODO:
    return NULL;
}

int server_start() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        eprintf("create Socket Failed!");
    }

    struct sockaddr_in servaddr;
    bool binded = false;
    for (int cur_port = port; cur_port <= port + port_range; cur_port++) {
        memset(&servaddr, 0, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(cur_port);
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) == -1) {
            logw("can not bind to port %d!", cur_port);
        } else {
            binded = true;
            port = cur_port;
            break;
        }
    }
    if (!binded) {
        eprintf("can not start server.");
    }

    if (listen(sockfd, USER_CNT) == -1) {
        eprintf("fail to listen on socket.");
    } else {
        log("listen on port %d.", port);
    }

    return sockfd;
}

void terminate_process(int signum) {
    for (int i = 0; i < USER_CNT; i++) {
        if (sessions[i].conn >= 0) {
            log("send quit to user #%d %s\033[2m(%s)\033[0m", i, sessions[i].user_name, sessions[i].ip_addr);
            if (signum) {
                send_to_client(
                    i, SERVER_STATUS_QUIT,
                    sformat(" (runtime error: %s)", signal_name_s[signum]));
            } else {
                send_to_client(i, SERVER_STATUS_QUIT);
            }
            log("close conn:%d", sessions[i].conn);
            close(sessions[i].conn);
            sessions[i].conn = -1;
        }
    }

    if (server_fd) {
        close(server_fd);
        log("close server fd:%d", server_fd);
    }

    pthread_mutex_destroy(&sessions_lock);
    pthread_mutex_destroy(&battles_lock);
    for (int i = 0; i < USER_CNT; i++) {
        pthread_mutex_destroy(&items_lock[i]);
    }

    log("exit(%d)", signum);
    exit(signum);
}

void terminate_entrance(int signum) {
    loge("received signal %s, terminate.", signal_name_s[signum]);
    terminate_process(signum == SIGINT ? 0 : signum);
}

int main(int argc, char* argv[]) {
    init_constants();
    init_handler();
    if (argc == 2) {
        port = atoi(argv[1]);
    }
    srand(time(NULL));

    pthread_t thread;

    if (signal(SIGINT, terminate_entrance) == SIG_ERR) {
        eprintf("an error occurred while setting a signal handler.");
    }
    if (signal(SIGSEGV, terminate_entrance) == SIG_ERR) {
        eprintf("an error occurred while setting a signal handler.");
    }
    if (signal(SIGABRT, terminate_entrance) == SIG_ERR) {
        eprintf("an error occurred while setting a signal handler.");
    }
    if (signal(SIGTERM, terminate_entrance) == SIG_ERR) {
        eprintf("an error occurred while setting a signal handler.");
    }
    if (signal(SIGTRAP, terminate_entrance) == SIG_ERR) {
        eprintf("an error occurred while setting a signal handler.");
    }

    for (int i = 0; i < USER_CNT; i++) {
        pthread_mutex_init(&items_lock[i], NULL);
    }
    log("server %s", version);
    if (sizeof(server_message_t) >= 1000)
        logw("message_size = %ldB", sizeof(server_message_t));

    server_fd = server_start();
    load_user_list();

    for (int i = 0; i < USER_CNT; i++)
        sessions[i].conn = -1;

    struct sockaddr_in client_addr;
    socklen_t length = sizeof(client_addr);
    while (1) {
        static session_args_t info;
        info.conn = accept(server_fd, (struct sockaddr*)&client_addr, &length);
        strncpy(info.ip_addr, inet_ntoa(client_addr.sin_addr), IPADDR_SIZE - 1);
        log("connected by %s:%d , conn:%d", info.ip_addr, client_addr.sin_port, info.conn);
        if (info.conn < 0) {
            loge("fail to accept client.");
        } else if (pthread_create(&thread, NULL, session_start, (void*)(uintptr_t)&info) != 0) {
            loge("fail to create thread.");
        }
        logi("bind thread #%lu", thread);
    }

    return 0;
}
