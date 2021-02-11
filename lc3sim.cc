#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/termios.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <vector>

enum Register {
  kR0 = 0,
  kR1,
  kR2,
  kR3,
  kR4,
  kR5,
  kR6,
  kR7,
  kPC,  // program counter
  kCOND,
  kRegisterCount
};

enum OpCode {
  kBR = 0,  // branch
  kADD,     // add
  kLD,      // load
  kST,      // store
  kJSR,     // jump register
  kAND,     // bitwise and
  kLDR,     // load register
  kSTR,     // store register
  kRTI,     // unused
  kNOT,     // bitwise not
  kLDI,     // load indirect
  kSTI,     // store indirect
  kJMP,     // jump
  kRES,     // reserved (unused)
  kLEA,     // load effective address
  kTRAP     // execute trap
};

enum Flag {
  kPositive = 1 << 0,  // P
  kZero = 1 << 1,      // Z
  kNegative = 1 << 2,  // N
};

enum MMIO {
  kKBSR = 0xFE00,  // keyboard status
  kKBDR = 0xFE02   // keyboard data
};

enum TrapCode {
  kGETC = 0x20,   // get character from keyboard, not echoed onto the terminal
  kOUT = 0x21,    // output a character
  kPUTS = 0x22,   // output a word string
  kIN = 0x23,     // get character from keyboard, echoed onto the terminal
  kPUTSP = 0x24,  // output a byte string
  kHALT = 0x25    // halt the program
};

uint16_t SignExtend(uint16_t x, int bitCount) {
  if ((x >> (bitCount - 1)) & 1) {
    x |= (0xFFFF << bitCount);
  }
  return x;
}

uint16_t Swap16(uint16_t x) { return (x << 8) | (x >> 8); }

void CloseFile(std::FILE *fp) { std::fclose(fp); };

constexpr uint16_t kPCStart = 0x3000;

class Simulator {
 public:
  Simulator() {
    // set the program counter to starting position
    registers_[kPC] = kPCStart;
  }

  Simulator(const Simulator &) = delete;
  Simulator &operator=(Simulator &) = delete;

  void UpdateFlags(uint16_t r) {
    if (registers_[r] == 0) {
      registers_[kCOND] = kZero;
    } else if (registers_[r] >>
               15)  // a 1 in the left-most bit indicates negative
    {
      registers_[kCOND] = kNegative;
    } else {
      registers_[kCOND] = kPositive;
    }
  }

  bool ReadImage(const std::string &filename) {
    std::unique_ptr<std::FILE, decltype(&CloseFile)> fp(
        std::fopen(filename.c_str(), "rb"), &CloseFile);
    uint16_t memory_origin;
    std::fread(&memory_origin, sizeof(memory_origin), 1, fp.get());
    memory_origin = Swap16(memory_origin);

    uint16_t remaining_memory_size = UINT16_MAX - memory_origin;
    uint16_t *p = &memory_[0] + memory_origin;
    auto nread =
        std::fread(p, sizeof(uint16_t), remaining_memory_size, fp.get());
    while (nread-- > 0) {
      *p = Swap16(*p);
      ++p;
    }
    return true;
  }

  uint16_t CheckKeyInput() {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &read_fds, NULL, NULL, &timeout) != 0;
  }

  void WriteMemory(uint16_t address, uint16_t x) { memory_[address] = x; }

  uint16_t ReadMemory(uint16_t address) {
    if (address == kKBSR) {
      if (CheckKeyInput()) {
        memory_[kKBSR] = (1 << 15);
        auto c = std::getchar();
        memory_[kKBDR] = static_cast<uint16_t>(c);
      } else {
        memory_[kKBSR] = 0;
      }
    }
    return memory_[address];
  }

  void Run() {
    bool running = true;
    while (running) {
      uint16_t instr = ReadMemory(registers_[kPC]++);
      uint16_t op = instr >> 12;

      switch (op) {
        case kADD: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t r1 = (instr >> 6) & 0x7;
          bool immediate = (instr >> 5) & 0x1;
          if (immediate) {
            uint16_t imm5 = SignExtend(instr & 0x1F, 5);
            registers_[r0] = registers_[r1] + imm5;
          } else {
            uint16_t r2 = instr & 0x7;
            registers_[r0] = registers_[r1] + registers_[r2];
          }
          UpdateFlags(r0);
        } break;

        case kAND: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t r1 = (instr >> 6) & 0x7;
          bool immediate = (instr >> 5) & 0x1;
          if (immediate) {
            uint16_t imm5 = SignExtend(instr & 0x1F, 5);
            registers_[r0] = registers_[r1] & imm5;
          } else {
            uint16_t r2 = instr & 0x7;
            registers_[r0] = registers_[r1] & registers_[r2];
          }
          UpdateFlags(r0);
        } break;

        case kNOT: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t r1 = (instr >> 6) & 0x7;
          registers_[r0] = ~registers_[r1];
          UpdateFlags(r0);
        } break;

        case kBR: {
          uint16_t pc_offset = SignExtend(instr & 0x1FF, 9);
          uint16_t condition = (instr >> 9) & 0x7;
          if (condition & registers_[kCOND]) {
            registers_[kPC] += pc_offset;
          }
        } break;

        case kJMP: {
          uint16_t r1 = (instr >> 6) & 0x7;
          registers_[kPC] = registers_[r1];
        } break;

        case kJSR: {
          bool long_flag = (instr >> 11) & 1;
          registers_[kR7] = registers_[kPC];
          if (long_flag) {  // JSR
            uint16_t longpc_offset = SignExtend(instr & 0x7FF, 11);
            registers_[kPC] += longpc_offset;
          } else {  // JSRR
            uint16_t r1 = (instr >> 6) & 0x7;
            registers_[kPC] += registers_[r1];
          }
        } break;

        case kLD: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pc_offset = SignExtend(instr & 0x1FF, 9);
          registers_[r0] = ReadMemory(registers_[kPC] + pc_offset);
          UpdateFlags(r0);
        } break;

        case kLDI: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pc_offset = SignExtend(instr & 0x1FF, 9);
          registers_[r0] = ReadMemory(ReadMemory(registers_[kPC] + pc_offset));
          UpdateFlags(r0);
        } break;

        case kLDR: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t r1 = (instr >> 6) & 0x7;
          uint16_t pc_offset = SignExtend(instr & 0x3F, 6);
          registers_[r0] = ReadMemory(registers_[r1] + pc_offset);
          UpdateFlags(r0);
        } break;

        case kLEA: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pc_offset = SignExtend(instr & 0x1FF, 9);
          registers_[r0] = registers_[kPC] + pc_offset;
          UpdateFlags(r0);
        } break;

        case kST: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pc_offset = SignExtend(instr & 0x1FF, 9);
          WriteMemory(registers_[kPC] + pc_offset, registers_[r0]);
        } break;

        case kSTI: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pc_offset = SignExtend(instr & 0x1FF, 9);
          WriteMemory(ReadMemory(registers_[kPC] + pc_offset), registers_[r0]);
        } break;

        case kSTR: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t r1 = (instr >> 6) & 0x7;
          uint16_t offset = SignExtend(instr & 0x3F, 6);
          WriteMemory(registers_[r1] + offset, registers_[r0]);
        } break;

        case kTRAP: {
          switch (instr & 0xFF) {
            case kGETC: {
              auto c = std::getchar();
              registers_[kR0] = static_cast<uint16_t>(c);
            } break;

            case kOUT: {
              std::putchar(static_cast<char>(registers_[kR0]));
              std::fflush(stdout);
            } break;

            case kPUTS: {
              uint16_t *c = &memory_[0] + registers_[kR0];
              while (*c) {
                std::putchar(static_cast<char>(*c));
                ++c;
              }
              std::fflush(stdout);
            } break;

            case kIN: {
              std::cout << "Enter a character: ";
              auto c = std::getchar();
              std::putchar(c);
              registers_[kR0] = static_cast<uint16_t>(c);
            } break;

            case kPUTSP: {
              uint16_t *c = &memory_[0] + registers_[kR0];
              while (*c) {
                char c1 = static_cast<char>((*c) & 0xFF);
                std::putchar(c1);
                char c2 = static_cast<char>((*c) >> 8);
                if (c2) {
                  std::putchar(c2);
                }
                ++c;
              }
              std::fflush(stdout);
            } break;

            case kHALT: {
              std::puts("HALT");
              std::fflush(stdout);
              running = false;
            } break;
          }
          break;
        }

        case kRES:
        case kRTI:
        default:
          std::abort();
          break;
      }
    }
  }

 private:
  std::array<uint16_t, UINT16_MAX> memory_;
  std::array<uint16_t, Register::kRegisterCount> registers_;
};

struct termios original_termios;

void DisableInputBuffering() {
  tcgetattr(STDIN_FILENO, &original_termios);
  struct termios newTermios = original_termios;
  newTermios.c_lflag &= ~ICANON & ~ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &newTermios);
}

void RestoreInputBuffering() {
  tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
}

void HandleInterrupt(int signal) {
  RestoreInputBuffering();
  std::cerr << std::endl;
  std::exit(-2);
}

void ShowUsage(const std::string &program) {
  std::cerr << "usage: " << program << " [option] ... [IMAGE] ...\n"
            << "Options and arguments:\n"
            << "\t-h, --help\t\tShow this help message" << std::endl;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    ShowUsage(argv[0]);
    std::exit(2);
  }

  std::vector<std::string> images;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      ShowUsage(argv[0]);
      std::exit(2);
    }
    images.push_back(arg);
  }

  Simulator sim;
  for (auto &image : images) {
    if (!sim.ReadImage(image)) {
      exit(2);
    }
  }

  signal(SIGINT, HandleInterrupt);
  DisableInputBuffering();

  sim.Run();

  RestoreInputBuffering();

  return 0;
}