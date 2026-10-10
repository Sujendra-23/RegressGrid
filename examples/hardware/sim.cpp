#include "Valu.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include <cstdint>
#include <iostream>
#include <string>
int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  Verilated::traceEverOn(true);
  const std::string suite = argc > 1 ? argv[1] : "add";
  Valu alu;
  VerilatedVcdC trace;
  alu.trace(&trace, 99);
  trace.open((suite + ".vcd").c_str());
  unsigned time = 0, failed = 0;
  for (unsigned a = 0; a < 256; a += 17)
    for (unsigned b = 0; b < 256; b += 19) {
      const unsigned begin = suite == "add"        ? 0
                             : suite == "subtract" ? 1
                                                   : 2,
                     end = suite == "bitwise" ? 4 : begin + 1;
      for (unsigned op = begin; op < end; op++) {
        alu.a = a;
        alu.b = b;
        alu.op = op;
        alu.eval();
        trace.dump(time++);
        const std::uint8_t want = op == 0   ? a + b
                                  : op == 1 ? a - b
                                  : op == 2 ? a & b
                                            : a ^ b;
        if (alu.result != want) {
          ++failed;
          std::cerr << "vector failed: a=" << a << " b=" << b << " op=" << op
                    << " expected=" << unsigned(want)
                    << " actual=" << unsigned(alu.result) << '\n';
        }
      }
    }
  trace.close();
  alu.final();
  std::cout << time << " vectors, " << failed << " failures\n";
  return failed ? 1 : 0;
}
