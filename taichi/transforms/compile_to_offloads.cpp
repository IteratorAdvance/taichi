#include "taichi/ir/ir.h"
#include "taichi/ir/transforms.h"
#include "taichi/ir/analysis.h"
#include "taichi/ir/visitors.h"
#include "taichi/program/compile_config.h"
#include "taichi/program/extension.h"
#include "taichi/program/kernel.h"

TLANG_NAMESPACE_BEGIN

namespace irpass {
namespace {

std::function<void(const std::string &)> make_pass_printer(bool verbose,
                                                           IRNode *ir) {
  if (!verbose) {
    return [](const std::string &) {};
  }
  return [ir, kn = ir->get_kernel()->name](const std::string &pass) {
    TI_INFO("[{}] {}:", kn, pass);
    std::cout << std::flush;
    irpass::re_id(ir);
    irpass::print(ir);
    std::cout << std::flush;
  };
}

}  // namespace

void compile_to_offloads(IRNode *ir,
                         const CompileConfig &config,
                         bool verbose,
                         bool vectorize,
                         bool grad,
                         bool ad_use_stack) {
  TI_AUTO_PROF;

  auto print = make_pass_printer(verbose, ir);
  print("Initial IR");

  if (grad) {
    irpass::reverse_segments(ir);
    print("Segment reversed (for autodiff)");
  }

  irpass::lower_ast(ir);
  print("Lowered");

  irpass::type_check(ir);
  print("Typechecked");
  irpass::analysis::verify(ir);

  if (ir->get_kernel()->is_evaluator) {
    TI_ASSERT(!grad);

    irpass::demote_operations(ir);
    print("Operations demoted");

    irpass::offload(ir);
    print("Offloaded");
    irpass::analysis::verify(ir);
    return;
  }

  if (vectorize) {
    irpass::loop_vectorize(ir);
    print("Loop Vectorized");
    irpass::analysis::verify(ir);

    irpass::vector_split(ir, config.max_vector_width, config.serial_schedule);
    print("Loop Split");
    irpass::analysis::verify(ir);
  }

  // TODO: strictly enforce bit vectorization for x86 cpu and CUDA now
  //       create a separate CompileConfig flag for the new pass
  if (arch_is_cpu(config.arch) || config.arch == Arch::cuda) {
    irpass::bit_loop_vectorize(ir);
    irpass::type_check(ir);
    print("Bit Loop Vectorized");
    irpass::analysis::verify(ir);
  }

  irpass::full_simplify(ir, false);
  print("Simplified I");
  irpass::analysis::verify(ir);

  if (grad) {
    // Remove local atomics here so that we don't have to handle their gradients
    irpass::demote_atomics(ir);

    irpass::full_simplify(ir, false);
    irpass::auto_diff(ir, ad_use_stack);
    irpass::full_simplify(ir, false);
    print("Gradient");
    irpass::analysis::verify(ir);
  }

  if (config.check_out_of_bound) {
    irpass::check_out_of_bound(ir);
    print("Bound checked");
    irpass::analysis::verify(ir);
  }

  irpass::flag_access(ir);
  print("Access flagged I");
  irpass::analysis::verify(ir);

  irpass::full_simplify(ir, false);
  print("Simplified II");
  irpass::analysis::verify(ir);

  irpass::offload(ir);
  print("Offloaded");
  irpass::analysis::verify(ir);

  // TODO: This pass may be redundant as cfg_optimization() is already called
  //  in full_simplify().
  if (config.cfg_optimization) {
    irpass::cfg_optimization(ir, false);
    print("Optimized by CFG");
    irpass::analysis::verify(ir);
  }

  irpass::flag_access(ir);
  print("Access flagged II");

  irpass::full_simplify(ir, /*after_lower_access=*/false);
  print("Simplified III");
  irpass::analysis::verify(ir);
}

void offload_to_executable(IRNode *ir,
                           const CompileConfig &config,
                           bool verbose,
                           bool lower_global_access,
                           bool make_thread_local,
                           bool make_block_local) {
  TI_AUTO_PROF;

  auto print = make_pass_printer(verbose, ir);

  // TODO: This is just a proof that we can demote struct-fors after offloading.
  // Eventually we might want the order to be TLS/BLS -> demote struct-for.
  // For now, putting this after TLS will disable TLS, because it can only
  // handle range-fors at this point.

  print("Start offload_to_executable");
  irpass::analysis::verify(ir);

  if (config.detect_read_only) {
    irpass::detect_read_only(ir);
    print("Detect read-only accesses");
  }

  irpass::demote_atomics(ir);
  print("Atomics demoted I");
  irpass::analysis::verify(ir);

  if (config.demote_dense_struct_fors) {
    irpass::demote_dense_struct_fors(ir);
    irpass::type_check(ir);
    print("Dense struct-for demoted");
    irpass::analysis::verify(ir);
  }

  if (make_thread_local) {
    irpass::make_thread_local(ir);
    print("Make thread local");
  }

  if (make_block_local) {
    irpass::make_block_local(ir);
    print("Make block local");
  }

  irpass::demote_atomics(ir);
  print("Atomics demoted II");
  irpass::analysis::verify(ir);

  std::unordered_map<OffloadedStmt *,
                     std::unordered_map<const SNode *, GlobalPtrStmt *>>
      uniquely_accessed_bit_structs;
  if (is_extension_supported(config.arch, Extension::quant) &&
      ir->get_config().quant_opt_atomic_demotion) {
    uniquely_accessed_bit_structs =
        irpass::analysis::gather_uniquely_accessed_bit_structs(ir);
  }

  irpass::remove_range_assumption(ir);
  print("Remove range assumption");

  irpass::remove_loop_unique(ir);
  print("Remove loop_unique");
  irpass::analysis::verify(ir);

  if (lower_global_access) {
    irpass::lower_access(ir, true);
    print("Access lowered");
    irpass::analysis::verify(ir);

    irpass::die(ir);
    print("DIE");
    irpass::analysis::verify(ir);

    irpass::flag_access(ir);
    print("Access flagged III");
    irpass::analysis::verify(ir);
  }

  irpass::demote_operations(ir);
  print("Operations demoted");

  irpass::full_simplify(ir, lower_global_access);
  print("Simplified IV");

  if (is_extension_supported(config.arch, Extension::quant)) {
    irpass::optimize_bit_struct_stores(ir, uniquely_accessed_bit_structs);
    print("Bit struct stores optimized");
  }

  // Final field registration correctness & type checking
  irpass::type_check(ir);
  irpass::analysis::verify(ir);
}

void compile_to_executable(IRNode *ir,
                           const CompileConfig &config,
                           bool vectorize,
                           bool grad,
                           bool ad_use_stack,
                           bool verbose,
                           bool lower_global_access,
                           bool make_thread_local,
                           bool make_block_local) {
  TI_AUTO_PROF;

  compile_to_offloads(ir, config, verbose, vectorize, grad, ad_use_stack);

  offload_to_executable(ir, config, verbose, lower_global_access,
                        make_thread_local, make_block_local);
}

}  // namespace irpass

TLANG_NAMESPACE_END
