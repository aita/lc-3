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
  kCondition,
  kRegisterCount
};

enum OpCode {
  kBranch = 0,            // branch
  kAdd,                   // add
  kLoad,                  // load
  kStore,                 // store
  kJumpRegister,          // jump register
  kBitwiseAnd,            // bitwise and
  kLoadRegister,          // load register
  kStoreRegister,         // store register
  kReturnFromInterrupt,   // unused
  kBitwiseNot,            // bitwise not
  kLoadIndirect,          // load indirect
  kStoreIndirect,         // store indirect
  kJump,                  // jump
  kReserved,              // reserved (unused)
  kLoadEffectiveAddress,  // load effective address
  kTrap                   // execute trap
};

enum Flag {
  kPositive = 1 << 0,  // P
  kZero = 1 << 1,      // Z
  kNegative = 1 << 2,  // N
};

enum MMIO {
  KeyboardStatus = 0xFE00,  // keyboard status
  KeyboardData = 0xFE02     // keyboard data
};

enum TrapCode {
  kGetC = 0x20,   // get character from keyboard, not echoed onto the terminal
  kOut = 0x21,    // output a character
  kPutS = 0x22,   // output a word string
  kIn = 0x23,     // get character from keyboard, echoed onto the terminal
  kPutSP = 0x24,  // output a byte string
  kHalt = 0x25    // halt the program
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
      registers_[kCondition] = kZero;
    } else if (registers_[r] >>
               15)  // a 1 in the left-most bit indicates negative
    {
      registers_[kCondition] = kNegative;
    } else {
      registers_[kCondition] = kPositive;
    }
  }

  bool ReadImage(const std::string &filename) {
    std::unique_ptr<std::FILE, decltype(&CloseFile)> fp(
        std::fopen(filename.c_str(), "rb"), &CloseFile);
    uint16_t memoryOrigin;
    std::fread(&memoryOrigin, sizeof(memoryOrigin), 1, fp.get());
    memoryOrigin = Swap16(memoryOrigin);

    uint16_t remainingMemorySize = UINT16_MAX - memoryOrigin;
    uint16_t *p = &memory_[0] + memoryOrigin;
    auto nread = std::fread(p, sizeof(uint16_t), remainingMemorySize, fp.get());
    while (nread-- > 0) {
      *p = Swap16(*p);
      ++p;
    }
    return true;
  }

  uint16_t CheckKeyInput() {
    fd_set readFDs;
    FD_ZERO(&readFDs);
    FD_SET(STDIN_FILENO, &readFDs);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &readFDs, NULL, NULL, &timeout) != 0;
  }

  void WriteMemory(uint16_t address, uint16_t x) { memory_[address] = x; }

  uint16_t ReadMemory(uint16_t address) {
    if (address == KeyboardStatus) {
      if (CheckKeyInput()) {
        memory_[KeyboardStatus] = (1 << 15);
        auto c = std::getchar();
        memory_[KeyboardData] = static_cast<uint16_t>(c);
      } else {
        memory_[KeyboardStatus] = 0;
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
        case kAdd: {
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

        case kBitwiseAnd: {
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

        case kBitwiseNot: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t r1 = (instr >> 6) & 0x7;
          registers_[r0] = ~registers_[r1];
          UpdateFlags(r0);
        } break;

        case kBranch: {
          uint16_t pcOffset = SignExtend(instr & 0x1FF, 9);
          uint16_t condition = (instr >> 9) & 0x7;
          if (condition & registers_[kCondition]) {
            registers_[kPC] += pcOffset;
          }
        } break;

        case kJump: {
          uint16_t r1 = (instr >> 6) & 0x7;
          registers_[kPC] = registers_[r1];
        } break;

        case kJumpRegister: {
          bool longFlag = (instr >> 11) & 1;
          registers_[kR7] = registers_[kPC];
          if (longFlag) {  // JSR
            uint16_t longPCOffset = SignExtend(instr & 0x7FF, 11);
            registers_[kPC] += longPCOffset;
          } else {  // JSRR
            uint16_t r1 = (instr >> 6) & 0x7;
            registers_[kPC] += registers_[r1];
          }
        } break;

        case kLoad: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pcOffset = SignExtend(instr & 0x1FF, 9);
          registers_[r0] = ReadMemory(registers_[kPC] + pcOffset);
          UpdateFlags(r0);
        } break;

        case kLoadIndirect: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pcOffset = SignExtend(instr & 0x1FF, 9);
          registers_[r0] = ReadMemory(ReadMemory(registers_[kPC] + pcOffset));
          UpdateFlags(r0);
        } break;

        case kLoadRegister: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t r1 = (instr >> 6) & 0x7;
          uint16_t pcOffset = SignExtend(instr & 0x3F, 6);
          registers_[r0] = ReadMemory(registers_[r1] + pcOffset);
          UpdateFlags(r0);
        } break;

        case kLoadEffectiveAddress: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pcOffset = SignExtend(instr & 0x1FF, 9);
          registers_[r0] = registers_[kPC] + pcOffset;
          UpdateFlags(r0);
        } break;

        case kStore: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pcOffset = SignExtend(instr & 0x1FF, 9);
          WriteMemory(registers_[kPC] + pcOffset, registers_[r0]);
        } break;

        case kStoreIndirect: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t pcOffset = SignExtend(instr & 0x1FF, 9);
          WriteMemory(ReadMemory(registers_[kPC] + pcOffset), registers_[r0]);
        } break;

        case kStoreRegister: {
          uint16_t r0 = (instr >> 9) & 0x7;
          uint16_t r1 = (instr >> 6) & 0x7;
          uint16_t offset = SignExtend(instr & 0x3F, 6);
          WriteMemory(registers_[r1] + offset, registers_[r0]);
        } break;

        case kTrap: {
          switch (instr & 0xFF) {
            case kGetC: {
              auto c = std::getchar();
              registers_[kR0] = static_cast<uint16_t>(c);
            } break;

            case kOut: {
              std::putchar(static_cast<char>(registers_[kR0]));
              std::fflush(stdout);
            } break;

            case kPutS: {
              uint16_t *c = &memory_[0] + registers_[kR0];
              while (*c) {
                std::putchar(static_cast<char>(*c));
                ++c;
              }
              std::fflush(stdout);
            } break;

            case kIn: {
              std::cout << "Enter a character: ";
              auto c = std::getchar();
              std::putchar(c);
              registers_[kR0] = static_cast<uint16_t>(c);
            } break;

            case kPutSP: {
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

            case kHalt: {
              std::puts("HALT");
              std::fflush(stdout);
              running = false;
            } break;
          }
          break;
        }

        case kReserved:
        case kReturnFromInterrupt:
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

struct termios originalTermios;

void DisableInputBuffering() {
  tcgetattr(STDIN_FILENO, &originalTermios);
  struct termios newTermios = originalTermios;
  newTermios.c_lflag &= ~ICANON & ~ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &newTermios);
}

void RestoreInputBuffering() {
  tcsetattr(STDIN_FILENO, TCSANOW, &originalTermios);
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