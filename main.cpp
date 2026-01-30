#include <iostream>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <cmath>
#include <string>
#include <vector>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <dirent.h>
#include <chrono>
#include <algorithm>
#include <sstream>

std::atomic<int> AIMBOT_FPS(60);
std::atomic<bool> AIMBOT_ENABLED(true);

int mem_fd = -1;
std::atomic<uint64_t> my_base(0);
std::vector<uint64_t> enemy_bases;
std::mutex enemy_mutex;
std::atomic<bool> running(true);

#define MY_X_OFF -0xA8
#define MY_Y_OFF -0xA4
#define MY_Z_OFF -0xA0
#define MY_YAW_OFF -0xC0
#define MY_PITCH_OFF -0xBC
#define ENEMY_X_OFF -0x41C
#define ENEMY_Y_OFF -0x418
#define ENEMY_Z_OFF -0x414

#define MY_QWORD 4575657222478978089ULL
#define ENEMY_QWORD 4369572502465989837ULL

struct Region {
    uint64_t start;
    uint64_t end;
};

// BATCH READ 3 FLOATS - 1 SYSCALL
inline void rf3(uint64_t a, float& x, float& y, float& z) {
    float buf[3];
    pread(mem_fd, buf, 12, a);
    x = buf[0];
    y = buf[1];
    z = buf[2];
}

// BATCH WRITE 2 FLOATS - 1 SYSCALL
inline void wf2(uint64_t a, float v1, float v2) {
    float buf[2] = {v1, v2};
    pwrite(mem_fd, buf, 8, a);
}

// SINGLE READ
inline float rf(uint64_t a) {
    float v;
    pread(mem_fd, &v, 4, a);
    return v;
}

// FASTEST PID FINDER
inline int find_pid() {
    DIR* d = opendir("/proc");
    if (!d) return 0;
    
    struct dirent* e;
    char b[512];
    
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        
        int pid = atoi(e->d_name);
        if (pid < 5000) continue;
        
        char p[256];
        sprintf(p, "/proc/%d/cmdline", pid);
        int f = open(p, O_RDONLY);
        if (f < 0) continue;
        
        ssize_t l = read(f, b, 511);
        close(f);
        
        if (l > 0) {
            b[l] = 0;
            if (strstr(b, "BlockmanGo")) {
                closedir(d);
                return pid;
            }
        }
    }
    closedir(d);
    return 0;
}

// SCANNER WORKER
void scan_worker(std::vector<Region>* regions, int start_idx, int end_idx, 
                 std::vector<uint64_t>* local_enemies, std::atomic<uint64_t>* found_player) {
    const size_t CHUNK = 1048576;
    unsigned char* buf = new unsigned char[CHUNK];
    
    for (int idx = start_idx; idx < end_idx && running; idx++) {
        uint64_t start = (*regions)[idx].start;
        uint64_t end = (*regions)[idx].end;
        
        for (uint64_t pos = start; pos < end && running; pos += CHUNK) {
            size_t sz = (pos + CHUNK > end) ? (end - pos) : CHUNK;
            ssize_t rd = pread(mem_fd, buf, sz, pos);
            if (rd < 8) continue;
            
            for (ssize_t i = 0; i <= rd - 8; i += 4) {
                uint64_t qw = *(uint64_t*)(buf + i);
                uint64_t addr = pos + i;
                
                if (qw == MY_QWORD) {
                    uint64_t expected = 0;
                    found_player->compare_exchange_strong(expected, addr);
                }
                
                if (qw == ENEMY_QWORD) {
                    local_enemies->push_back(addr);
                }
            }
        }
    }
    
    delete[] buf;
}

// MASTER SCANNER - AUTO DETECTS NEW REGIONS
void master_scanner(int pid) {
    bool first = true;
    
    while (running) {
        auto t1 = std::chrono::high_resolution_clock::now();
        
        // ALWAYS GET FRESH REGION LIST
        char path[64];
        sprintf(path, "/proc/%d/maps", pid);
        FILE* f = fopen(path, "r");
        if (!f) {
            usleep(50000);
            continue;
        }
        
        std::vector<Region> regions;
        char line[512];
        
        while (fgets(line, 512, f)) {
            if (strstr(line, "[anon:libc_malloc]") && strstr(line, "rw")) {
                uint64_t start, end;
                sscanf(line, "%lx-%lx", &start, &end);
                regions.push_back({start, end});
            }
        }
        fclose(f);
        
        if (regions.empty()) {
            usleep(50000);
            continue;
        }
        
        // CHECK IF MY QWORD STILL VALID
        uint64_t mb = my_base.load();
        if (mb != 0) {
            uint64_t test_qw;
            pread(mem_fd, &test_qw, 8, mb);
            if (test_qw != MY_QWORD) {
                std::cout << "🔄 Server hop detected - Rescanning\n";
                my_base = 0;
                first = true;
            }
        }
        
        // 6 PARALLEL WORKERS
        const int WORKERS = 6;
        int total = regions.size();
        int per_thread = (total + WORKERS - 1) / WORKERS;
        
        std::vector<std::thread> workers;
        std::vector<std::vector<uint64_t>> local_lists(WORKERS);
        std::atomic<uint64_t> found_p(0);
        
        for (int i = 0; i < WORKERS; i++) {
            int start = i * per_thread;
            int end = std::min(start + per_thread, total);
            
            if (start < total) {
                workers.emplace_back(scan_worker, &regions, start, end, 
                                   &local_lists[i], &found_p);
            }
        }
        
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
        
        auto t2 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
        
        uint64_t pa = found_p.load();
        if (pa != 0) {
            my_base = pa;
            
            if (first) {
                std::cout << "Player: " << (ms.count() / 1000.0f) << "ms\n";
            }
        }
        
        // MERGE ALL ENEMIES
        std::vector<uint64_t> all_enemies;
        for (auto& list : local_lists) {
            all_enemies.insert(all_enemies.end(), list.begin(), list.end());
        }
        
        uint64_t pb = my_base.load();
        
        // VALIDATE ENEMIES - REMOVE INVALID QWORDS
        std::vector<uint64_t> valid_enemies;
        for (auto& eb : all_enemies) {
            if (eb == pb) continue;
            
            uint64_t test_qw;
            pread(mem_fd, &test_qw, 8, eb);
            if (test_qw == ENEMY_QWORD) {
                valid_enemies.push_back(eb);
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(enemy_mutex);
            enemy_bases = std::move(valid_enemies);
        }
        
        if (first && pb != 0) {
            std::cout << "All players: " << (ms.count() / 1000.0f) << "ms\n";
            std::cout << "Regions: " << regions.size() << "\n\n";
            first = false;
        }
        
        // FAST RESCAN - 200ms
        for (int i = 0; i < 2 && running; i++) usleep(100000);
    }
}

// FASTEST MATH THREAD
std::atomic<bool> need_calc(false);
float calc_mx, calc_my, calc_mz, calc_ex, calc_ey, calc_ez;
std::atomic<float> result_yaw(0), result_pitch(0);

void math_thread() {
    while (running) {
        if (!need_calc.load()) {
            usleep(50);
            continue;
        }
        
        // FASTEST MATH
        float dx = calc_ex - calc_mx;
        float dy = calc_ey - calc_my - 0.5f;
        float dz = calc_ez - calc_mz;
        
        float h = sqrtf(dx*dx + dz*dz + 0.0001f);
        
        float p = -atan2f(dy, h) * 57.29577951308232f;
        float y = atan2f(dz, dx) * 57.29577951308232f - 90.0f;
        
        // FAST NORMALIZE
        if (y > 180.0f) y -= 360.0f;
        else if (y < -180.0f) y += 360.0f;
        
        // FAST CLAMP
        if (p > 90.0f) p = 90.0f;
        else if (p < -90.0f) p = -90.0f;
        
        result_yaw = y;
        result_pitch = p;
        need_calc = false;
    }
}

// FASTEST AIMBOT - BATCH READS/WRITES
void aimbot() {
    while (running) {
        auto start = std::chrono::high_resolution_clock::now();
        
        if (!AIMBOT_ENABLED.load()) {
            usleep(10000);
            continue;
        }
        
        int current_fps = AIMBOT_FPS.load();
        int64_t frame_ns = 1000000000 / current_fps;
        
        uint64_t mb = my_base.load();
        if (mb == 0) {
            usleep(1000);
            continue;
        }
        
        // BATCH READ MY POSITION - 1 SYSCALL
        float mx, my, mz;
        rf3(mb + MY_X_OFF, mx, my, mz);
        
        if (!std::isfinite(mx)) {
            usleep(500);
            continue;
        }
        
        std::vector<uint64_t> ce;
        {
            std::lock_guard<std::mutex> lock(enemy_mutex);
            ce = enemy_bases;
        }
        
        if (ce.empty()) {
            usleep(1000);
            continue;
        }
        
        // FIND CLOSEST - NO SQRT
        float md = 999999.0f;
        float tx = 0, ty = 0, tz = 0;
        bool fnd = false;
        
        for (auto& eb : ce) {
            // BATCH READ ENEMY - 1 SYSCALL
            float ex, ey, ez;
            rf3(eb + ENEMY_X_OFF, ex, ey, ez);
            
            if (!std::isfinite(ex) || ex > 1e6f || ex < -1e6f) continue;
            
            // SQUARED DISTANCE
            float dx = ex - mx;
            float dy = ey - my;
            float dz = ez - mz;
            float d = dx*dx + dy*dy + dz*dz;
            
            if (d < md && d > 0.01f && d < 250000.0f) {
                md = d;
                tx = ex;
                ty = ey;
                tz = ez;
                fnd = true;
            }
        }
        
        if (fnd) {
            // SEND TO MATH
            calc_mx = mx;
            calc_my = my;
            calc_mz = mz;
            calc_ex = tx;
            calc_ey = ty;
            calc_ez = tz;
            need_calc = true;
            
            // WAIT
            int wait = 0;
            while (need_calc.load() && wait < 50) {
                usleep(10);
                wait++;
            }
            
            // BATCH WRITE - 1 SYSCALL (FIXED)
            wf2(mb + MY_YAW_OFF, result_yaw.load(), result_pitch.load());
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        
        if (elapsed < frame_ns) {
            usleep((frame_ns - elapsed) / 1000);
        }
    }
}

// CONTROL THREAD
void control_thread() {
    std::string line;
    
    while (running) {
        std::cout << ">> ";
        std::getline(std::cin, line);
        
        if (line.empty()) continue;
        
        if (line == "stop" || line == "quit" || line == "exit") {
            running = false;
            std::cout << "Stopping...\n";
            break;
        }
        
        if (line == "ab") {
            bool current = AIMBOT_ENABLED.load();
            AIMBOT_ENABLED = !current;
            std::cout << (AIMBOT_ENABLED.load() ? "✅ Aimbot ON\n" : "❌ Aimbot OFF\n");
            continue;
        }
        
        if (line == "list") {
            std::cout << "\n📋 COMMANDS:\n";
            std::cout << "  fps <num>  - Change FPS\n";
            std::cout << "  ab         - Toggle aimbot\n";
            std::cout << "  list       - Show commands\n";
            std::cout << "  stop       - Exit\n\n";
            continue;
        }
        
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        
        if (cmd == "fps") {
            int new_fps;
            iss >> new_fps;
            
            if (new_fps > 0 && new_fps <= 10000) {
                AIMBOT_FPS = new_fps;
                std::cout << "✅ FPS: " << new_fps << " (" << (1000.0f/new_fps) << "ms)\n";
            } else {
                std::cout << "❌ Invalid FPS\n";
            }
        } else {
            std::cout << "Type 'list' for commands\n";
        }
    }
}

int main() {
    std::cout << "🔥 NUCLEAR AIMBOT - LIGHT SPEED\n\n";
    
    auto t1 = std::chrono::high_resolution_clock::now();
    int pid = find_pid();
    auto t2 = std::chrono::high_resolution_clock::now();
    
    if (!pid) {
        std::cout << "❌ Game not found\n";
        return 1;
    }
    
    std::cout << "PID: " << pid << " | " 
              << std::chrono::duration_cast<std::chrono::milliseconds>(t2-t1).count() << "ms\n";
    
    char mp[64];
    sprintf(mp, "/proc/%d/mem", pid);
    mem_fd = open(mp, O_RDWR);
    
    if (mem_fd < 0) {
        std::cout << "❌ Memory failed\n";
        return 1;
    }
    
    auto t3 = std::chrono::high_resolution_clock::now();
    
    char path[64];
    sprintf(path, "/proc/%d/maps", pid);
    FILE* maps = fopen(path, "r");
    char line[512];
    int regions = 0;
    
    while (fgets(line, 512, maps)) {
        if (strstr(line, "[anon:libc_malloc]") && strstr(line, "rw")) {
            regions++;
        }
    }
    fclose(maps);
    
    auto t4 = std::chrono::high_resolution_clock::now();
    
    std::cout << "Regions: " << regions << " | " 
              << std::chrono::duration_cast<std::chrono::milliseconds>(t4-t3).count() << "ms\n\n";
    
    std::cout << "⚡ Starting at " << AIMBOT_FPS.load() << " FPS\n";
    std::cout << "Type 'list' for commands\n\n";
    
    std::thread ts(master_scanner, pid);
    std::thread tm(math_thread);
    std::thread ta(aimbot);
    std::thread tc(control_thread);
    
    if (ts.joinable()) ts.join();
    if (tm.joinable()) tm.join();
    if (ta.joinable()) ta.join();
    if (tc.joinable()) tc.join();
    
    if (mem_fd >= 0) close(mem_fd);
    
    std::cout << "✅ Done\n";
    return 0;
}