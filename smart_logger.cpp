#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <ctime>
#include <cmath>
#include <cstring>
#include <modbus.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <termios.h>

// ===========================
// CALIBRATION CONSTANTS
// ===========================
const int CALIBRATION_REG_MODE = 13;        // Calibration mode register
const int CALIBRATION_REG_COEFF = 28;       // Calibration coefficient register (float)
const float CALIBRATION_COEFF_VALUE = 12880;  // Standard EC calibration value
const uint16_t CAL_MODE_1_VALUE = 2;        // Mode 1: Write value 2 to register 13
const uint16_t CAL_MODE_2_VALUE = 3;        // Mode 2: Write value 3 to register 13

enum CalibrationMode {
    CAL_MODE_NONE = 0,    // Skip calibration
    CAL_MODE_1 = 1,       // Mode 1: Register 13 = 2
    CAL_MODE_2 = 2,       // Mode 2: Register 28 = 12.880, Register 13 = 3
    CAL_MODE_3 = 3        // Mode 3: TEST - Write K=190 to Register 16
};

// ===========================
// DYNAMIC COEFFICIENT LOOKUP
// ===========================
double get_dynamic_k(double temp) {
    if (temp <= 5.0) {
        return 0.0180;  // 1.80%
    } else if (temp <= 10.0) {
        return 0.0184;  // 1.84%
    } else if (temp <= 15.0) {
        return 0.0190;  // 1.90%
    } else if (temp <= 25.0) {
        return 0.0190;  // 1.90% (flat range)
    } else if (temp <= 30.0) {
        return 0.0192;  // 1.92%
    } else {
        return 0.0194;  // 1.94%
    }
}

// ===========================
// SMART ALGORITHM
// ===========================
double calculate_smart_ec(double raw_ec, double temp) {
    double k = get_dynamic_k(temp);
    // C25 = raw_ec / (1 + k * (temp - 25))
    return raw_ec / (1.0 + k * (temp - 25.0));
}

// ===========================
// PORT AUTO-DISCOVERY
// ===========================
std::string find_sensor_port() {
    std::vector<std::string> ports;
    
    // Scan /dev/ttyS0 through /dev/ttyS20 (WSL1/Legacy mode)
    for (int i = 0; i <= 20; i++) {
        ports.push_back("/dev/ttyS" + std::to_string(i));
    }
    
    // Also scan USB ports in case user switches to WSL2 USB passthrough
    for (int i = 0; i < 5; i++) {
        ports.push_back("/dev/ttyUSB" + std::to_string(i));
        ports.push_back("/dev/ttyACM" + std::to_string(i));
    }
    
    std::cout << "🔍 Scanning ports for BOQU IOT-485-EC4A (Slave ID: 4)..." << std::endl;
    
    uint16_t test_reg[2];
    
    for (const auto &port : ports) {
        modbus_t *ctx = modbus_new_rtu(port.c_str(), 9600, 'N', 8, 1);
        if (ctx == NULL) continue;
        
        modbus_set_slave(ctx, 4);  // CRITICAL: Slave ID 4, not 1
        modbus_set_response_timeout(ctx, 0, 100000);  // 100ms timeout
        
        if (modbus_connect(ctx) != -1) {
            // Try to read temperature register (60-61) as handshake
            int rc = modbus_read_registers(ctx, 60, 2, test_reg);
            
            if (rc != -1) {
                std::cout << "✅ FOUND SENSOR at: " << port << std::endl;
                modbus_close(ctx);
                modbus_free(ctx);
                return port;
            }
            modbus_close(ctx);
        }
        modbus_free(ctx);
    }
    
    return "";
}

// ===========================
// FLOAT CONVERSION (ABCD Big Endian)
// ===========================
float modbus_get_float_abcd(const uint16_t *src) {
    // ABCD format: [AB][CD] -> Big Endian
    uint32_t i;
    float f;
    
    // Combine two 16-bit registers into one 32-bit value
    // src[0] contains high word (AB), src[1] contains low word (CD)
    i = (((uint32_t)src[0]) << 16) | src[1];
    
    // Reinterpret as float
    memcpy(&f, &i, sizeof(float));
    
    return f;
}

// ===========================
// HEX STRING CONVERTER (For Data Validation)
// ===========================
// Converts two 16-bit Modbus registers to an 8-character hex string.
// This allows validation of IEEE 754 float conversion by logging the raw bytes.
// Example: reg_high=0x4135 (16693), reg_low=0x1A86 (6790) → "41351A86"
// You can verify this at: https://www.h-schmidt.net/FloatConverter/IEEE754.html
std::string to_hex_string(uint16_t reg_high, uint16_t reg_low) {
    std::stringstream ss;
    // Use std::hex to switch to hexadecimal mode
    // std::uppercase for capital letters (A-F)
    // std::setfill('0') ensures leading zeros are preserved
    // std::setw(4) ensures each 16-bit value outputs exactly 4 hex characters
    ss << std::uppercase << std::hex << std::setfill('0')
       << std::setw(4) << reg_high
       << std::setw(4) << reg_low;
    return ss.str();
}

// ===========================
// MODBUS WRITE: SINGLE INTEGER REGISTER
// ===========================
bool write_integer_register(modbus_t *ctx, int reg_addr, uint16_t value) {
    // Show what we're about to write
    std::cout << "  [WRITE] Sending to Register " << reg_addr
              << ": Value=" << value
              << " (0x" << std::hex << std::uppercase << value << std::dec << ")\n";

    // Write the register
    int rc = modbus_write_register(ctx, reg_addr, value);
    if (rc == -1) {
        std::cerr << "  [ERROR] Failed to write register " << reg_addr
                  << ": " << modbus_strerror(errno) << std::endl;
        return false;
    }

    // Read back to verify
    uint16_t verify_value;
    if (modbus_read_registers(ctx, reg_addr, 1, &verify_value) != -1) {
        std::cout << "  [VERIFY] Read back from Register " << reg_addr
                  << ": Value=" << verify_value
                  << " (0x" << std::hex << std::uppercase << verify_value << std::dec << ")\n";

        if (verify_value == value) {
            std::cout << "  [OK] Write verified successfully!\n";
        } else {
            std::cerr << "  [WARNING] Read-back value differs! Expected " << value
                      << ", got " << verify_value << "\n";
        }
    } else {
        std::cerr << "  [WARNING] Could not verify write (read-back failed)\n";
    }

    return true;
}

// ===========================
// MODBUS WRITE: FLOAT VALUE (2 REGISTERS, ABCD FORMAT)
// ===========================
// Note: A 32-bit float requires 2 consecutive 16-bit registers.
// When writing to Register 28, it automatically uses Register 29 too.
// This is standard Modbus behavior (same as Modbus Poll).
bool write_float_register(modbus_t *ctx, int reg_addr, float value) {
    uint16_t reg_data[2];

    // Convert float to ABCD format (Big Endian, matches sensor's format)
    modbus_set_float_abcd(value, reg_data);

    // Show what we're about to write
    std::cout << "  [WRITE] Float " << std::fixed << std::setprecision(3) << value
              << " -> Register " << reg_addr << " (uses " << reg_addr << "-" << (reg_addr + 1) << " internally)\n";
    std::cout << "          Hex: " << to_hex_string(reg_data[0], reg_data[1])
              << " (Reg" << reg_addr << "=0x" << std::hex << std::uppercase << reg_data[0]
              << ", Reg" << (reg_addr + 1) << "=0x" << reg_data[1] << std::dec << ")\n";

    // Write 2 consecutive registers (starting at reg_addr)
    int rc = modbus_write_registers(ctx, reg_addr, 2, reg_data);
    if (rc == -1) {
        std::cerr << "  [ERROR] Failed to write float to register " << reg_addr
                  << ": " << modbus_strerror(errno) << std::endl;
        return false;
    }

    // Read back to verify
    uint16_t verify_data[2];
    usleep(100000);  // 100ms delay for sensor to process

    if (modbus_read_registers(ctx, reg_addr, 2, verify_data) != -1) {
        float read_back = modbus_get_float_abcd(verify_data);
        std::cout << "  [VERIFY] Reading back from Register " << reg_addr << "...\n";
        std::cout << "          Read: " << std::fixed << std::setprecision(3) << read_back
                  << " (Hex: " << to_hex_string(verify_data[0], verify_data[1]) << ")\n";

        if (fabs(read_back - value) < 0.001f) {
            std::cout << "  [OK] Write verified successfully!\n";
        } else {
            std::cerr << "  [WARNING] Read-back value differs! Expected " << value
                      << ", got " << read_back << "\n";
        }
    } else {
        std::cerr << "  [WARNING] Could not verify write (read-back failed)\n";
    }

    return true;
}

// ===========================
// EXECUTE CALIBRATION SEQUENCE
// ===========================
bool execute_calibration(modbus_t *ctx, CalibrationMode mode) {
    if (mode == CAL_MODE_NONE) {
        std::cout << "  [INFO] Calibration skipped (mode 0)" << std::endl;
        return true;
    }

    std::cout << "\n";
    std::cout << "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓\n";
    std::cout << "┃               CALIBRATION MODE " << static_cast<int>(mode) << " EXECUTION                           ┃\n";
    std::cout << "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛\n\n";

    bool success = true;

    if (mode == CAL_MODE_1) {
        // Mode 1: Write Register 13 = 2
        std::cout << "  Mode 1: Writing calibration mode value...\n";
        success = write_integer_register(ctx, CALIBRATION_REG_MODE, CAL_MODE_1_VALUE);

    } else if (mode == CAL_MODE_2) {
        // Mode 2: Write Register 28 = 12.880, then Register 13 = 3
        std::cout << "  Mode 2: Writing calibration coefficient...\n";
        success = write_float_register(ctx, CALIBRATION_REG_COEFF, CALIBRATION_COEFF_VALUE);

        if (success) {
            std::cout << "  Mode 2: Writing calibration mode value...\n";
            success = write_integer_register(ctx, CALIBRATION_REG_MODE, CAL_MODE_2_VALUE);
        }

    } else if (mode == CAL_MODE_3) {
        // Mode 3: TEST writing K value to Register 16
        std::cout << "  Mode 3: TESTING K coefficient write to Register 16...\n";
        std::cout << "  Writing K=0.0190 scaled to 190 (K x 10000)...\n";
        uint16_t test_k = 190;  // 0.0190 * 10000
        success = write_integer_register(ctx, 16, test_k);

        if (success) {
            std::cout << "\n  SUCCESS! Sensor accepts K x 10000 format.\n";
            std::cout << "  You can now enable auto-K in the main loop.\n";
        } else {
            std::cout << "\n  FAILED! Sensor may not accept this format.\n";
            std::cout << "  Try K x 1000 (value=19) instead.\n";
        }
    }

    if (success) {
        std::cout << "\n  Calibration Mode " << static_cast<int>(mode) << " completed successfully!\n\n";
    } else {
        std::cerr << "\n  Calibration failed! Check sensor connection.\n\n";
    }

    // Give sensor time to process calibration
    sleep(1);

    return success;
}

// ===========================
// GET CALIBRATION MODE FROM USER/ARGS
// ===========================
CalibrationMode get_calibration_mode(int argc, char* argv[]) {
    // Check for command-line argument
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            int mode = std::atoi(argv[i + 1]);
            if (mode >= 0 && mode <= 3) {
                std::cout << "  Using calibration mode " << mode << " from command line.\n";
                return static_cast<CalibrationMode>(mode);
            } else {
                std::cerr << "  Invalid mode '" << mode << "'. Using interactive selection.\n";
            }
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "\nUsage: ./smart_logger [OPTIONS]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --mode 0    Skip calibration\n";
            std::cout << "  --mode 1    Calibration Mode 1: Register 13 = 2\n";
            std::cout << "  --mode 2    Calibration Mode 2: Register 28 = 12.880, Register 13 = 3\n";
            std::cout << "  --mode 3    TEST Mode: Write K=190 to Register 16 (test x10000 format)\n";
            std::cout << "  --help      Show this help message\n\n";
            exit(0);
        }
    }

    // Interactive mode selection
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║              SELECT CALIBRATION MODE                                  ║\n";
    std::cout << "╠═══════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  [0] Skip calibration (use existing sensor settings)                  ║\n";
    std::cout << "║  [1] Mode 1: Write Register 13 = 2 (integer)                          ║\n";
    std::cout << "║  [2] Mode 2: Write Register 28 = 12.880 (float) + Register 13 = 3     ║\n";
    std::cout << "║  [3] Mode 3: TEST - Write K=190 to Register 16 (test x10000 format)   ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n  Enter mode (0/1/2/3): ";

    int choice;
    std::cin >> choice;

    if (choice >= 0 && choice <= 3) {
        return static_cast<CalibrationMode>(choice);
    }

    std::cout << "  Invalid choice. Defaulting to Mode 0 (skip).\n";
    return CAL_MODE_NONE;
}

// ===========================
// CLEAR SCREEN (Cross-platform)
// ===========================
void clear_screen() {
    #ifdef _WIN32
        system("cls");
    #else
        system("clear");
    #endif
}

// ===========================
// GET TIMESTAMP
// ===========================
std::string get_timestamp() {
    time_t now = time(0);
    struct tm tstruct;
    char buf[80];
    tstruct = *localtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tstruct);
    return buf;
}

// ===========================
// DISPLAY SENSOR DIAGNOSTIC REGISTERS (REAL-TIME LOOP)
// ===========================
void display_sensor_diagnostics(modbus_t *ctx) {
    int loop_count = 0;

    std::cout << "\n  Starting real-time diagnostic monitor...\n";
    std::cout << "  Press ENTER to stop monitoring and proceed to calibration.\n\n";
    sleep(2);

    // Set stdin to non-blocking mode
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    while (true) {
        loop_count++;
        clear_screen();

        std::cout << "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓\n";
        std::cout << "┃         SENSOR DIAGNOSTIC REGISTERS (REAL-TIME)                   ┃\n";
        std::cout << "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛\n\n";

        std::cout << "  Time: " << get_timestamp() << "  |  Updates: " << loop_count << "\n\n";

        uint16_t reg_value;
        uint16_t reg_data[2];

        // Read Register 1
        if (modbus_read_registers(ctx, 1, 1, &reg_value) != -1) {
            std::cout << "  Register  1 = " << std::setw(5) << reg_value
                      << "  (0x" << std::hex << std::uppercase << std::setfill('0')
                      << std::setw(4) << reg_value << std::dec << std::setfill(' ') << ")\n";
        } else {
            std::cout << "  Register  1 = [READ ERROR]\n";
        }

        // Read Register 2
        if (modbus_read_registers(ctx, 2, 1, &reg_value) != -1) {
            std::cout << "  Register  2 = " << std::setw(5) << reg_value
                      << "  (0x" << std::hex << std::uppercase << std::setfill('0')
                      << std::setw(4) << reg_value << std::dec << std::setfill(' ') << ")\n";
        } else {
            std::cout << "  Register  2 = [READ ERROR]\n";
        }

        // Read Register 16
        if (modbus_read_registers(ctx, 16, 1, &reg_value) != -1) {
            std::cout << "  Register 16 = " << std::setw(5) << reg_value
                      << "  (0x" << std::hex << std::uppercase << std::setfill('0')
                      << std::setw(4) << reg_value << std::dec << std::setfill(' ') << ")\n";
        } else {
            std::cout << "  Register 16 = [READ ERROR]\n";
        }

        std::cout << "\n  ─── Calibration Registers ───\n\n";

        // Register 13 (calibration mode)
        if (modbus_read_registers(ctx, 13, 1, &reg_value) != -1) {
            std::cout << "  Register 13 = " << std::setw(5) << reg_value
                      << "  (0x" << std::hex << std::uppercase << std::setfill('0')
                      << std::setw(4) << reg_value << std::dec << std::setfill(' ')
                      << ")  <- Calibration Mode\n";
        } else {
            std::cout << "  Register 13 = [READ ERROR]  <- Calibration Mode\n";
        }

        // Register 28 as float (calibration coefficient)
        if (modbus_read_registers(ctx, 28, 2, reg_data) != -1) {
            float coeff = modbus_get_float_abcd(reg_data);
            std::cout << "  Register 28 = " << std::fixed << std::setprecision(3) << coeff
                      << "  (Hex: " << to_hex_string(reg_data[0], reg_data[1])
                      << ")  <- Calibration Coefficient\n";
        } else {
            std::cout << "  Register 28 = [READ ERROR]  <- Calibration Coefficient\n";
        }

        std::cout << "\n┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄\n";
        std::cout << "  Use these values to verify sensor state.\n";
        std::cout << "  >>> Press ENTER to proceed to calibration mode selection <<<\n";

        std::cout.flush();

        // Check if user pressed a key
        char c;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == '\n' || c == '\r' || c == ' ') {
                break;  // Exit loop on Enter or Space
            }
        }

        sleep(1);  // Update every 1 second
    }

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    std::cout << "\n  Diagnostic monitoring stopped.\n\n";
}

// ===========================
// TEACHER MODE: GET TEMPERATURE CONDITION
// ===========================
std::string get_temp_condition(double temp) {
    if (temp <= 5.0) {
        return "Very Cold Range (≤5°C)";
    } else if (temp <= 10.0) {
        return "Cold Range (5-10°C)";
    } else if (temp <= 15.0) {
        return "Cool Range (10-15°C)";
    } else if (temp <= 25.0) {
        return "Normal Range (15-25°C)";
    } else {
        return "Warm Range (>25°C)";
    }
}

// ===========================
// TEACHER MODE: DISPLAY EDUCATIONAL DASHBOARD
// ===========================
void display_teacher_dashboard(double temp, double raw_ec, double sensor_ec, double smart_ec, 
                               double k_used, int sample_count, const std::string &port,
                               const std::string &hex_temp, const std::string &hex_raw_ec) {
    clear_screen();
    
    // Calculate validation metrics
    const double STANDARD_VALUE = 12.88;
    double sensor_error = fabs(sensor_ec - STANDARD_VALUE);
    double smart_error = fabs(smart_ec - STANDARD_VALUE);
    double improvement = sensor_error - smart_error;
    
    // Determine pass/fail
    const double TOLERANCE = 0.10;  // ±0.10 mS/cm tolerance
    bool sensor_pass = sensor_error <= TOLERANCE;
    bool smart_pass = smart_error <= TOLERANCE;
    
    std::cout << "╔═══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           🎓 TEACHER MODE: LIVE ALGORITHM VALIDATION 🎓              ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════════╝\n\n";
    
    std::cout << "  📡 Port: " << port << " | Samples: " << sample_count 
              << " | Time: " << get_timestamp() << "\n\n";
    
    // ========== SECTION A: THE "WHY" (LOGIC DISPLAY) ==========
    std::cout << "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓\n";
    std::cout << "┃ 📚 SECTION A: THE \"WHY\" - Understanding the Logic                   ┃\n";
    std::cout << "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛\n\n";
    
    std::cout << "  Current Condition:\n";
    std::cout << "    🌡️  Measured Temperature = " << std::fixed << std::setprecision(2) 
              << temp << "°C  (0x" << hex_temp << ")  →  " << get_temp_condition(temp) << "\n\n";
    
    std::cout << "  Decision Logic:\n";
    std::cout << "    🧠 Therefore, using Dynamic Coefficient k = " << std::setprecision(4) 
              << k_used << " (" << (k_used * 100) << "%)\n";
    std::cout << "    🔴 Sensor uses FIXED Coefficient k = 0.0200 (2.00%) ← WRONG!\n\n";
    
    std::cout << "  Why This Matters:\n";
    std::cout << "    • At low temps, sensor OVER-compensates (k too high)\n";
    std::cout << "    • Our algorithm adjusts k based on actual calibration data\n";
    std::cout << "    • Result: More accurate readings across temperature range\n\n";
    
    // ========== SECTION B: THE MATH (FORMULA VISUALIZATION) ==========
    std::cout << "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓\n";
    std::cout << "┃ 🧮 SECTION B: THE MATH - Live Formula Calculation                   ┃\n";
    std::cout << "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛\n\n";
    
    std::cout << "  Temperature Compensation Formula:\n\n";
    std::cout << "    C₂₅ = Raw_EC / (1 + k × (Temp - 25))\n\n";
    
    std::cout << "  Sensor's Calculation (FIXED k=0.02):\n";
    std::cout << "    " << std::setprecision(2) << sensor_ec << " = " << raw_ec 
              << " / (1 + 0.0200 × (" << temp << " - 25.0))\n";
    std::cout << "    " << sensor_ec << " = " << raw_ec << " / " 
              << std::setprecision(4) << (1.0 + 0.02 * (temp - 25.0)) << "\n\n";
    
    std::cout << "  Smart Algorithm (DYNAMIC k=" << std::setprecision(4) << k_used << "):\n";
    std::cout << "    " << std::setprecision(2) << smart_ec << " = " << raw_ec 
              << " / (1 + " << std::setprecision(4) << k_used << " × (" 
              << std::setprecision(2) << temp << " - 25.0))\n";
    std::cout << "    " << smart_ec << " = " << raw_ec << " / " 
              << std::setprecision(4) << (1.0 + k_used * (temp - 25.0)) << "\n\n";
    
    // ========== SECTION C: THE VERDICT (VALIDATION) ==========
    std::cout << "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓\n";
    std::cout << "┃ ⚖️  SECTION C: THE VERDICT - Validation Against Standard            ┃\n";
    std::cout << "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛\n\n";
    
    std::cout << "  Standard Reference: 12.88 mS/cm @ 25°C\n";
    std::cout << "  Tolerance: ±" << TOLERANCE << " mS/cm\n\n";
    
    std::cout << "  Distance from Standard:\n";
    std::cout << "    🔴 Sensor Error:  " << std::setprecision(4) << std::setw(8) << sensor_error 
              << " mS/cm  ";
    if (sensor_pass) {
        std::cout << "✅ PASS\n";
    } else {
        std::cout << "❌ FAIL (exceeds tolerance)\n";
    }
    
    std::cout << "    🟢 Smart Error:   " << std::setw(8) << smart_error << " mS/cm  ";
    if (smart_pass) {
        std::cout << "✅ PASS\n";
    } else {
        std::cout << "❌ FAIL (exceeds tolerance)\n";
    }
    
    std::cout << "\n  Improvement Score:\n";
    std::cout << "    📈 Error Reduction: " << std::setprecision(4) << improvement << " mS/cm";
    
    if (improvement > 0) {
        std::cout << "  ✅ Smart Algorithm is BETTER!\n";
    } else if (improvement < 0) {
        std::cout << "  ⚠️  Sensor Default is better (rare)\n";
    } else {
        std::cout << "  ➡️  No difference\n";
    }
    
    std::cout << "    📊 Improvement: " << std::setprecision(1) 
              << (sensor_error > 0 ? (improvement / sensor_error * 100.0) : 0.0) << "%\n\n";
    
    // ========== SUMMARY BOX ==========
    std::cout << "┌───────────────────────────────────────────────────────────────────────┐\n";
    std::cout << "│                         📊 QUICK SUMMARY                              │\n";
    std::cout << "├───────────────────────────────────────────────────────────────────────┤\n";
    std::cout << "│  🌡️  Temperature:     " << std::setprecision(2) << std::setw(10) << temp << " °C";
    std::cout << "  [Hex: " << hex_temp << "]             │\n";
    std::cout << "│  📊 Raw EC:           " << std::setw(10) << raw_ec << " mS/cm";
    std::cout << "  [Hex: " << hex_raw_ec << "]             │\n";
    std::cout << "│  🔴 Sensor Output:    " << std::setw(10) << sensor_ec << " mS/cm  ";
    std::cout << (sensor_pass ? "✅ PASS" : "❌ FAIL") << "                    │\n";
    std::cout << "│  🟢 Smart Output:     " << std::setw(10) << smart_ec << " mS/cm  ";
    std::cout << (smart_pass ? "✅ PASS" : "❌ FAIL") << "                    │\n";
    std::cout << "└───────────────────────────────────────────────────────────────────────┘\n\n";
    
    std::cout << "  💾 Logging to CSV: ec_data_log.csv\n";
    std::cout << "  ⏹️  Press Ctrl+C to stop and analyze data\n\n";
}

// ===========================
// MAIN PROGRAM
// ===========================
int main(int argc, char* argv[]) {
    // Step 1: Auto-discover the sensor
    std::string port = find_sensor_port();
    
    if (port.empty()) {
        std::cerr << "❌ ERROR: Sensor not found!" << std::endl;
        std::cerr << "   Check: USB connection, Slave ID (must be 4), Baud Rate (9600)" << std::endl;
        return -1;
    }
    
    // Step 2: Establish main connection
    modbus_t *ctx = modbus_new_rtu(port.c_str(), 9600, 'N', 8, 1);
    if (ctx == NULL) {
        std::cerr << "❌ Failed to create Modbus context" << std::endl;
        return -1;
    }
    
    modbus_set_slave(ctx, 4);
    modbus_set_response_timeout(ctx, 1, 0);  // 1 second for main loop
    
    if (modbus_connect(ctx) == -1) {
        std::cerr << "❌ Connection failed: " << modbus_strerror(errno) << std::endl;
        modbus_free(ctx);
        return -1;
    }
    
    std::cout << "\n🚀 Connected to sensor on " << port << std::endl;
    std::cout << "📊 Starting Smart Logger..." << std::endl;
    std::cout << "📝 Data will be logged to: ec_data_log.csv" << std::endl;
    std::cout << "   Press Ctrl+C to stop.\n" << std::endl;

    // Step 2.5: Display sensor diagnostic registers
    display_sensor_diagnostics(ctx);

    // Step 2.6: Get calibration mode
    CalibrationMode cal_mode = get_calibration_mode(argc, argv);

    // Step 2.7: Execute calibration (after connection, before main loop)
    if (!execute_calibration(ctx, cal_mode)) {
        std::cerr << "⚠️  Calibration failed! Continuing with sensor defaults.\n";
    }

    sleep(1);
    
    // Step 3: Create/Open CSV file
    std::ofstream csv_file;
    bool file_exists = (access("ec_data_log.csv", F_OK) != -1);
    
    csv_file.open("ec_data_log.csv", std::ios::app);
    
    // Write header if new file (with hex validation columns)
    if (!file_exists) {
        csv_file << "Timestamp,Temperature,Hex_Temp,Raw_EC,Hex_Raw_EC,Sensor_Default_EC,Smart_Calc_EC,Deviation\n";
    }
    
    // Step 4: Main data acquisition loop
    uint16_t reg_data[2];
    int loop_count = 0;
    std::string hex_temp, hex_raw_ec;  // Raw hex strings for data validation
    
    while (true) {
        loop_count++;
        
        // Read Temperature (Reg 60-61)
        double temp = 0.0;
        if (modbus_read_registers(ctx, 60, 2, reg_data) != -1) {
            // Capture raw hex BEFORE float conversion for validation
            hex_temp = to_hex_string(reg_data[0], reg_data[1]);
            temp = modbus_get_float_abcd(reg_data);
        } else {
            std::cerr << "⚠️  Failed to read temperature" << std::endl;
            sleep(1);
            continue;
        }
        
        // Read Raw EC (Reg 45-46)
        double raw_ec = 0.0;
        if (modbus_read_registers(ctx, 45, 2, reg_data) != -1) {
            // Capture raw hex BEFORE float conversion for validation
            hex_raw_ec = to_hex_string(reg_data[0], reg_data[1]);
            raw_ec = modbus_get_float_abcd(reg_data);
        } else {
            std::cerr << "⚠️  Failed to read raw EC" << std::endl;
            sleep(1);
            continue;
        }
        
        // Read Sensor's Internal EC (Reg 41-42) - "The Wrong Value"
        double sensor_ec = 0.0;
        if (modbus_read_registers(ctx, 41, 2, reg_data) != -1) {
            sensor_ec = modbus_get_float_abcd(reg_data);
        } else {
            std::cerr << "⚠️  Failed to read sensor EC" << std::endl;
            sleep(1);
            continue;
        }
        
        // Calculate Smart EC
        double smart_ec = calculate_smart_ec(raw_ec, temp);
        double k_used = get_dynamic_k(temp);
        double deviation = sensor_ec - smart_ec;
        
        // Calculate validation metrics
        const double STANDARD_VALUE = 12.88;
        double distance_sensor = fabs(sensor_ec - STANDARD_VALUE);
        double distance_smart = fabs(smart_ec - STANDARD_VALUE);
        double improvement_score = distance_sensor - distance_smart;
        
        // Display educational dashboard (with hex validation data)
        display_teacher_dashboard(temp, raw_ec, sensor_ec, smart_ec, k_used, loop_count, port,
                                  hex_temp, hex_raw_ec);
        
        // Log to CSV with hex validation columns
        csv_file << get_timestamp() << ","
                 << temp << ","
                 << hex_temp << ","
                 << raw_ec << ","
                 << hex_raw_ec << ","
                 << sensor_ec << ","
                 << smart_ec << ","
                 << deviation << "\n";
        csv_file.flush();
        
        // Wait 1 second before next reading
        sleep(1);
    }
    
    // Cleanup (unreachable, but good practice)
    csv_file.close();
    modbus_close(ctx);
    modbus_free(ctx);
    
    return 0;
}
