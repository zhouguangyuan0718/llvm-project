// Generated CoreDSL LLVM 23 TableGen spike.
// This validation unit intentionally uses pseudo instructions and a synthetic
// register file; it proves def/Pat import, not final MC encoding.
include "llvm/Target/Target.td"

// Spike-only synthetic register file. Real register names, count, encodings,
// aliases and allocation properties still require target description data.
def TR0 : Register<"tr0">;
def TR1 : Register<"tr1">;
def TR2 : Register<"tr2">;
def TR3 : Register<"tr3">;
def X0 : Register<"x0">;

def TensorReg_v64i16 : RegisterClass<"CoreDSLSpike", [v64i16], 1024, (add TR0, TR1, TR2, TR3)>;
def GPR : RegisterClass<"CoreDSLSpike", [i64], 64, (add X0)>;

class CoreDSLSpikeInst<dag outs, dag ins, string asmstr> : Instruction {
  let Namespace = "CoreDSLSpike";
  let OutOperandList = outs;
  let InOperandList = ins;
  let AsmString = asmstr;
  let Pattern = [];
  let hasSideEffects = 0;
  let mayLoad = 0;
  let mayStore = 0;
  let isPseudo = 1;
}

// TADD remains pseudo in this spike: a final EncodingModel and target instruction
// format class are not available yet.
def TADD
    : CoreDSLSpikeInst<(outs TensorReg_v64i16:$dst),
                       (ins TensorReg_v64i16:$lhs, TensorReg_v64i16:$rhs),
                       "TADD $dst, $lhs, $rhs">;

def : Pat<(v64i16 (add (v64i16 TensorReg_v64i16:$lhs),
                                      (v64i16 TensorReg_v64i16:$rhs))),
          (TADD TensorReg_v64i16:$lhs, TensorReg_v64i16:$rhs)>;

// Not emitted: MATMUL: memory tensor needs pointer, dynamic extent/stride ABI, memory effects and a selection source.

defm : RemapAllTargetPseudoPointerOperands<GPR>;

def CoreDSLSpikeInstrInfo : InstrInfo;
def CoreDSLSpike : Target {
  let InstructionSet = CoreDSLSpikeInstrInfo;
}
