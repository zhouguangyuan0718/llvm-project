#include "ExampleLegalizerInfo.h"

#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <initializer_list>

using namespace llvm;

static cl::opt<bool> ExpectStrict("expect-strict", cl::init(false));

#ifdef HAVE_NATIVE_TARGET
void runMIRTests(bool Strict, bool Missing);
#endif

static void expect(const ExampleLegalizerInfo &LI, unsigned Opcode,
                   std::initializer_list<LLT> Types,
                   LegalizeActions::LegalizeAction Action, unsigned TypeIdx = 0,
                   LLT NewType = LLT{}) {
  const LegalityQuery Query(Opcode, ArrayRef<LLT>(Types));
  const auto Step = LI.getAction(Query);
  if (Step.Action != Action ||
      (NewType.isValid() &&
       (Step.TypeIdx != TypeIdx || Step.NewType != NewType))) {
    errs() << "Unexpected action for ";
    Query.print(errs());
    errs() << ": expected " << unsigned(Action) << ", got "
           << unsigned(Step.Action) << '\n';
    std::exit(1);
  }
}

static void checkMemory(const ExampleLegalizerInfo &LI, LLT Ty,
                        LegalizeActions::LegalizeAction Action) {
  const LLT Types[] = {Ty, LLT::pointer(0, 64)};
  const LegalityQuery::MemDesc Memory(Ty, 8, AtomicOrdering::NotAtomic,
                                      AtomicOrdering::NotAtomic);
  for (unsigned Opcode : {TargetOpcode::G_LOAD, TargetOpcode::G_STORE}) {
    const auto Step = LI.getAction({Opcode, Types, {Memory}});
    // These rules retain LLVM's empty legacy fallback, which can report
    // NotFound rather than Unsupported. Neither is a successful action.
    if (Step.Action != Action && !(Action == LegalizeActions::Unsupported &&
                                   Step.Action == LegalizeActions::NotFound)) {
      errs() << "Unexpected memory action for " << Ty << '\n';
      std::exit(1);
    }
  }
}

int main(int Argc, char **Argv) {
  cl::ParseCommandLineOptions(Argc, Argv);
  // Subtarget construction may precede enabling ExtendedLLT. Build the rules
  // first, then exercise them with the target's semantic types enabled.
  LLT::setUseExtended(false);
  ExampleLegalizerInfo LI;
  LLT::setUseExtended(true);
  using namespace TargetOpcode;
  using namespace LegalizeActions;
  const LLT I1 = LLT::integer(1), I8 = LLT::integer(8);
  const LLT I16 = LLT::integer(16), I32 = LLT::integer(32);
  const LLT I64 = LLT::integer(64);
  const LLT F16 = LLT::floatIEEE(16), F32 = LLT::floatIEEE(32);
  const LLT F64 = LLT::floatIEEE(64);
  const LLT P64 = LLT::pointer(0, 64);

  // The generic policy ignores both present and missing instruction entries.
  expect(LI, G_ADD, {I16}, ExpectStrict ? Unsupported : Legal);
  expect(LI, G_ADD, {I8}, ExpectStrict ? Unsupported : Custom);
  expect(LI, G_FADD, {F32}, ExpectStrict ? Unsupported : Legal);
  expect(LI, G_MUL, {I16}, Legal);
  expect(LI, G_MUL, {I32}, ExpectStrict ? Unsupported : Legal);
  expect(LI, G_MUL, {I8}, Custom);

  // Fixed artifact and intentional pass-through rules are not scalar fallback.
  expect(LI, G_ANYEXT, {I32, I8}, Legal);
  expect(LI, G_TRUNC, {I8, I32}, Legal);
  expect(LI, G_BR, {}, Legal);
  expect(LI, G_PTR_ADD, {P64, I8}, Legal);
  expect(LI, G_INTTOPTR, {P64, I8}, Legal);
  expect(LI, G_PTRTOINT, {I8, P64}, Legal);
  expect(LI, G_PTRMASK, {P64, I8}, Legal);
  expect(LI, G_GLOBAL_VALUE, {P64}, Legal);
  // The removed pointer comparison path must not masquerade as integer ICMP.
  expect(LI, G_ICMP, {I16, P64}, Unsupported);
  checkMemory(LI, P64, Legal);
  checkMemory(LI, LLT::fixed_vector(4, I16), Legal);

#ifdef MISSING_FIXTURE
  constexpr bool Missing = true;
#else
  constexpr bool Missing = false;
#endif

  if (!ExpectStrict) {
    // Identical native lists with different opcode/intrinsic lists must produce
    // identical generic rules. In particular, explicit exclusions are ignored.
    expect(LI, G_XOR, {I16}, Legal);
    expect(LI, G_CONSTANT, {I64}, Legal);
    expect(LI, G_FCONSTANT, {F32}, Legal);
    expect(LI, G_ICMP, {I64, I64}, Legal);
    expect(LI, G_FCMP, {I1, F16}, WidenScalar, 1, F32);
    expect(LI, G_FCMP, {I32, F32}, Legal);
    expect(LI, G_SHL, {I32, I16}, Legal);
    expect(LI, G_BRCOND, {I16}, Legal);
    expect(LI, G_BRCOND, {I64}, Legal);
    expect(LI, G_FMAXIMUM, {F64}, Legal);
    expect(LI, G_FMINIMUM, {F64}, Legal);
    expect(LI, G_SELECT, {I64, I16}, Custom);
    expect(LI, G_SELECT, {I32, I64}, Custom);
    checkMemory(LI, I32, Legal);
    checkMemory(LI, I8, Unsupported);
    checkMemory(LI, F32, Legal);
    checkMemory(LI, I1, Custom);
  } else {
#ifdef MISSING_FIXTURE
    expect(LI, G_CONSTANT, {I16}, ExpectStrict ? Unsupported : Legal);
    expect(LI, G_CONSTANT, {I8}, ExpectStrict ? Unsupported : Custom);
    expect(LI, G_FCONSTANT, {F32}, ExpectStrict ? Unsupported : Legal);
    expect(LI, G_ICMP, {I16, I16}, ExpectStrict ? Unsupported : Legal);
    expect(LI, G_ICMP, {I1, I8}, ExpectStrict ? Unsupported : Custom);
    expect(LI, G_FCMP, {I32, F32}, ExpectStrict ? Unsupported : Legal);
    expect(LI, G_SELECT, {I32, I16}, ExpectStrict ? Unsupported : Custom);
    checkMemory(LI, I1, ExpectStrict ? Unsupported : Custom);
#else
    // Explicit constants cannot bypass their operation constraints via
    // native_types.
    expect(LI, G_CONSTANT, {I16}, Legal);
    expect(LI, G_CONSTANT, {I8}, Custom);
    expect(LI, G_CONSTANT, {I64}, Unsupported);
    expect(LI, G_FCONSTANT, {F64}, Legal);
    expect(LI, G_FCONSTANT, {F32}, Custom);

    // ICMP intersects result [i16,i32] with input [i16,i32,i64].
    // In particular, native input i64 must not bypass the result constraint.
    expect(LI, G_ICMP, {I16, I16}, Legal);
    expect(LI, G_ICMP, {I1, I8}, Custom);
    expect(LI, G_ICMP, {I32, I32}, Legal);
    expect(LI, G_ICMP, {I64, I64}, Unsupported);
    // FCMP intersects result [i64] with input [f32,f64].
    expect(LI, G_FCMP, {I1, F16}, WidenScalar, 1, F64);
    expect(LI, G_FCMP, {I32, F32}, WidenScalar, 1, F64);
    expect(LI, G_FCMP, {I64, F64}, Legal);
    expect(LI, G_SHL, {I32, I16}, ExpectStrict ? Unsupported : Legal);
    expect(LI, G_BRCOND, {I32}, Legal);
    expect(LI, G_BRCOND, {I16}, Custom);
    expect(LI, G_BRCOND, {I64}, Unsupported);

    // FMAXIMUM follows the ordinary FMINIMUM rule, without a custom expansion.
    for (unsigned Opcode : {G_FMAXIMUM, G_FMINIMUM}) {
      expect(LI, Opcode, {F32}, Legal);
      expect(LI, Opcode, {F16}, Unsupported);
      expect(LI, Opcode, {F64}, Unsupported);
    }
    expect(LI, G_SELECT, {I32, I16}, Custom);
    expect(LI, G_SELECT, {I64, I16}, Unsupported); // No PHI carrier.
    expect(LI, G_SELECT, {I32, I64}, Unsupported); // No branch carrier.
    checkMemory(LI, I16, Legal);
    checkMemory(LI, I8, Unsupported);
    checkMemory(LI, F16, Bitcast);
    checkMemory(LI, I1, Custom);
#endif
  }

#ifdef HAVE_NATIVE_TARGET
  runMIRTests(ExpectStrict, Missing);
#endif
  outs() << (Missing ? "missing" : "configured") << " fixture, "
         << (ExpectStrict ? "strict" : "fallback") << " mode passed\n";
}
