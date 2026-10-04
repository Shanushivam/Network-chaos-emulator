#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>

#define CHAOS_MAGIC 'C'
#define CHAOS_SET_CONFIG _IOW(CHAOS_MAGIC, 1, struct chaos_config)
#define CHAOS_GET_STATS  _IOR(CHAOS_MAGIC, 2, struct chaos_stats)

struct chaos_config {
    int latency_ms;      
    int loss_rate;       
    int corrupt_rate;    
    int enable_ingress;  
    int enable_egress;   
};

struct chaos_stats {
    unsigned long total_packets;
    unsigned long dropped_packets;
    unsigned long delayed_packets;
    unsigned long corrupted_packets;
};

void print_menu() {
    std::cout << "\n====================================\n";
    std::cout << "    NETWORK CHAOS CONTROL CENTER    \n";
    std::cout << "====================================\n";
    std::cout << "1. Apply Chaos Rules (Latency, Loss, Corruption)\n";
    std::cout << "2. Clear All Rules (Normal Network)\n";
    std::cout << "3. Fetch Live Kernel Stats\n";
    std::cout << "4. Exit\n";
    std::cout << "Select Option [1-4]: ";
}

int main() {
    int fd = open("/dev/chaos_emulator", O_RDWR);
    if (fd < 0) {
        std::cerr << "[ERROR] Failed to open /dev/chaos_emulator. Did you run 'sudo insmod chaos_driver.ko' and 'sudo chmod 666 /dev/chaos_emulator'?\n";
        return 1;
    }

    int choice;
    while (true) {
        print_menu();
        if (!(std::cin >> choice)) break;

        if (choice == 1) {
            chaos_config config;
            config.enable_ingress = 1;
            config.enable_egress = 1;

            std::cout << "--> Enter Latency (ms): ";
            std::cin >> config.latency_ms;
            std::cout << "--> Enter Packet Loss Rate (0-100%): ";
            std::cin >> config.loss_rate;
            std::cout << "--> Enter Corruption Rate (0-100%): ";
            std::cin >> config.corrupt_rate;

            if (ioctl(fd, CHAOS_SET_CONFIG, &config) < 0) {
                std::cerr << "[ERROR] Failed to apply rules via ioctl.\n";
            } else {
                std::cout << "\n[SUCCESS] Chaos Rules Sent to Kernel Module!\n";
            }
        } else if (choice == 2) {
            chaos_config config = {0, 0, 0, 1, 1};
            if (ioctl(fd, CHAOS_SET_CONFIG, &config) < 0) {
                std::cerr << "[ERROR] Failed to clear rules.\n";
            } else {
                std::cout << "\n[SUCCESS] All Chaos Rules Cleared! Network Reset to Normal.\n";
            }
        } else if (choice == 3) {
            chaos_stats stats;
            if (ioctl(fd, CHAOS_GET_STATS, &stats) < 0) {
                std::cerr << "[ERROR] Failed to fetch stats.\n";
            } else {
                std::cout << "\n--- LIVE KERNEL TELEMETRY ---\n";
                std::cout << "Total Packets Intercepted : " << stats.total_packets << "\n";
                std::cout << "Packets Dropped           : " << stats.dropped_packets << "\n";
                std::cout << "Packets Delayed           : " << stats.delayed_packets << "\n";
                std::cout << "Packets Corrupted         : " << stats.corrupted_packets << "\n";
            }
        } else if (choice == 4) {
            std::cout << "Exiting Chaos Control Center.\n";
            break;
        } else {
            std::cout << "[!] Invalid Choice. Try again.\n";
        }
    }

    close(fd);
    return 0;
}
