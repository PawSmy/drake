#include <benchmark/benchmark.h>

#include "drake/common/drake_assert.h"
#include "drake/common/find_resource.h"
#include "drake/math/autodiff.h"
#include "drake/math/autodiff_gradient.h"
#include "drake/multibody/parsing/parser.h"
#include "drake/multibody/plant/multibody_plant.h"
#include "drake/tools/performance/fixture_common.h"

namespace drake {
namespace multibody {
namespace {

using math::RigidTransform;
using math::RollPitchYaw;
using symbolic::Expression;
using symbolic::MakeVectorVariable;
using systems::Context;
using systems::ContinuousState;
using systems::FixedInputPortValue;
using systems::System;

// We use this alias to silence cpplint barking at mutable references.
using BenchmarkStateRef = benchmark::State&;

// In the benchmark case instantiations at the bottom of this file, we'll use
// a bitmask for the case's "Arg" to denote which quantities are in scope as
// either gradients (for T=AutoDiffXd) or variables (for T=Expression).
constexpr int kWantNoGrad   = 0x0;
constexpr int kWantGradQ    = 0x1;
constexpr int kWantGradV    = 0x2;
constexpr int kWantGradX    = kWantGradQ | kWantGradV;
constexpr int kWantGradVdot = 0x4;
constexpr int kWantGradU    = 0x8;

// Fixture that holds a Cassie robot model and offers helper functions to
// configure the benchmark case.
template <typename T>
class Cassie : public benchmark::Fixture {
 public:
  Cassie() {
    tools::performance::AddMinMaxStatistics(this);
  }

  // This apparently futile using statement works around "overloaded virtual"
  // errors in g++. All of this is a consequence of the weird deprecation of
  // const-ref State versions of SetUp() and TearDown() in benchmark.h.
  using benchmark::Fixture::SetUp;
  void SetUp(BenchmarkStateRef state) override {
    SetUpNonZeroState();
    SetUpGradientsOrVariables(state);
  }

 protected:
  // Loads the plant.
  static std::unique_ptr<MultibodyPlant<T>> MakePlant();

  // Sets the plant to have non-zero state and input. In some cases, computing
  // using zeros will not tickle the relevant paths through the code.
  void SetUpNonZeroState();

  // In the benchmark case instantiations at the bottom of this file, we'll use
  // a bitmask for the case's "Arg" to denote which quantities are in scope as
  // either gradients (for T=AutoDiffXd) or variables (for T=Expression).
  static bool want_grad_q(const benchmark::State& state) {
    return state.range(0) & kWantGradQ;
  }
  static bool want_grad_v(const benchmark::State& state) {
    return state.range(0) & kWantGradV;
  }
  static bool want_grad_vdot(const benchmark::State& state) {
    return state.range(0) & kWantGradVdot;
  }
  static bool want_grad_u(const benchmark::State& state) {
    return state.range(0) & kWantGradU;
  }

  // Using the "Arg" from the given benchmark state, sets up the MbP
  // state and/or input to use gradients and/or symbolic variables
  // as configured in this benchmark case.
  //
  // For T=double, any request for gradients is an error.
  // For T=AutoDiffXd, sets the specified gradients to the identity matrix.
  // For T=Expression, sets the specified quantities to symbolic variables.
  void SetUpGradientsOrVariables(BenchmarkStateRef state);

  // Use these functions to invalidate input- or state-dependent computations
  // each benchmarked step. Disabling the cache entirely would affect the
  // performance because it would suppress any internal use of the cache during
  // complicated computations like forward dynamics. For example, if there are
  // multiple places in forward dynamics that access body positions, currently
  // those would get computed once and re-used (like in real applications) but
  // with caching off they would get recalculated repeatedly, affecting the
  // timing results.
  void InvalidateInput() {
    input_.GetMutableData();
  }
  void InvalidateState() {
    context_->NoteContinuousStateChange();
  }

  // Runs the MassMatrix benchmark.
  void DoMassMatrix(BenchmarkStateRef state) {
    DRAKE_DEMAND(want_grad_vdot(state) == false);
    DRAKE_DEMAND(want_grad_u(state) == false);
    for (auto _ : state) {
      InvalidateState();
      plant_->CalcMassMatrix(*context_, &mass_matrix_out_);
    }
  }

  // Runs the InverseDynamics benchmark.
  void DoInverseDynamics(BenchmarkStateRef state) {
    DRAKE_DEMAND(want_grad_u(state) == false);
    for (auto _ : state) {
      InvalidateState();
      plant_->CalcInverseDynamics(*context_, desired_vdot_, external_forces_);
    }
  }

  // Runs the ForwardDynamics benchmark.
  void DoForwardDynamics(BenchmarkStateRef state) {
    DRAKE_DEMAND(want_grad_vdot(state) == false);
    for (auto _ : state) {
      InvalidateInput();
      InvalidateState();
      plant_->EvalTimeDerivatives(*context_);
    }
  }

  // The plant itself.
  const std::unique_ptr<const MultibodyPlant<T>> plant_{MakePlant()};
  const int nq_{plant_->num_positions()};
  const int nv_{plant_->num_velocities()};
  const int nu_{plant_->num_actuators()};

  // The plant's context.
  std::unique_ptr<Context<T>> context_{plant_->CreateDefaultContext()};
  FixedInputPortValue& input_ = plant_->get_actuation_input_port().FixValue(
      context_.get(), VectorX<T>::Zero(nu_));

  // Data used in the MassMatrix cases (only).
  MatrixX<T> mass_matrix_out_;

  // Data used in the InverseDynamics cases (only).
  VectorX<T> desired_vdot_;
  MultibodyForces<T> external_forces_{*plant_};
};

using CassieDouble = Cassie<double>;
using CassieAutoDiff = Cassie<AutoDiffXd>;
using CassieExpression = Cassie<Expression>;

template <typename T>
std::unique_ptr<MultibodyPlant<T>> Cassie<T>::MakePlant() {
  auto plant = std::make_unique<MultibodyPlant<double>>(0.0);
  Parser parser(plant.get());
  const auto& model = "drake/multibody/benchmarking/cassie_v2.urdf";
  parser.AddModelFromFile(FindResourceOrThrow(model));
  plant->Finalize();
  if constexpr (std::is_same_v<T, double>) {
    return plant;
  } else {
    return System<double>::ToScalarType<T>(*plant);
  }
}

template <typename T>
void Cassie<T>::SetUpNonZeroState() {
  // Reset 'x'; be sure to set quaternions back to a sane value.
  context_->get_mutable_continuous_state_vector().SetFromVector(
      VectorX<T>::LinSpaced(nq_ + nv_, 0.1, 0.9));
  for (const BodyIndex& index : plant_->GetFloatingBaseBodies()) {
    const Body<T>& body = plant_->get_body(index);
    const RigidTransform<T> pose(
        RollPitchYaw<T>(0.1, 0.2, 0.3), Vector3<T>(0.4, 0.5, 0.6));
    plant_->SetFreeBodyPose(context_.get(), body, pose);
  }

  // Reset 'vdot'.
  desired_vdot_ = VectorX<T>::Constant(nv_, 0.5);

  // Reset 'u'.
  input_.GetMutableVectorData<T>()->SetFromVector(
      VectorX<T>::Constant(nu_, 0.5));

  // Reset 'tau'.
  external_forces_.mutable_generalized_forces() =
      VectorX<T>::LinSpaced(nv_, 0.01, 0.09);

  // Reset temporaries.
  mass_matrix_out_ = MatrixX<T>::Zero(nv_, nv_);
}

template <>
void Cassie<double>::SetUpGradientsOrVariables(BenchmarkStateRef state) {
  DRAKE_DEMAND(want_grad_q(state) == false);
  DRAKE_DEMAND(want_grad_v(state) == false);
  DRAKE_DEMAND(want_grad_vdot(state) == false);
  DRAKE_DEMAND(want_grad_u(state) == false);
}

template <>
void Cassie<AutoDiffXd>::SetUpGradientsOrVariables(BenchmarkStateRef state) {
  // For the quantities destined for InitializeAutoDiff, read their default
  // values (without any gradients). For the others, leave the matrix empty.
  VectorX<double> q, v, vdot, u;
  if (want_grad_q(state)) {
    q = math::DiscardGradient(plant_->GetPositions(*context_));
  }
  if (want_grad_v(state)) {
    v = math::DiscardGradient(plant_->GetVelocities(*context_));
  }
  if (want_grad_vdot(state)) {
    vdot = math::DiscardGradient(desired_vdot_);
  }
  if (want_grad_u(state)) {
    u = math::DiscardGradient(input_.get_vector_value<AutoDiffXd>().value());
  }

  // Initialize the desired gradients.
  VectorX<AutoDiffXd> q_grad, v_grad, vdot_grad, u_grad;
  std::tie(q_grad, v_grad, vdot_grad, u_grad) =
      math::InitializeAutoDiffTuple(q, v, vdot, u);

  // Write the gradients back to the plant.
  if (want_grad_q(state)) {
    plant_->SetPositions(context_.get(), q_grad);
  }
  if (want_grad_v(state)) {
    plant_->SetVelocities(context_.get(), v_grad);
  }
  if (want_grad_vdot(state)) {
    desired_vdot_ = vdot_grad;
  }
  if (want_grad_u(state)) {
    input_.GetMutableVectorData<AutoDiffXd>()->SetFromVector(u_grad);
  }
}

template <>
void Cassie<Expression>::SetUpGradientsOrVariables(BenchmarkStateRef state) {
  if (want_grad_q(state)) {
    const VectorX<Expression> q = MakeVectorVariable(nq_, "q");
    plant_->SetPositions(context_.get(), q);
  }
  if (want_grad_v(state)) {
    const VectorX<Expression> v = MakeVectorVariable(nv_, "v");
    plant_->SetVelocities(context_.get(), v);
  }
  if (want_grad_vdot(state)) {
    desired_vdot_ = MakeVectorVariable(nv_, "vd");
  }
  if (want_grad_u(state)) {
    const VectorX<Expression> u = MakeVectorVariable(nu_, "u");
    input_.GetMutableVectorData<Expression>()->SetFromVector(u);
  }
}

// All that remains is to add the sensible combinations of benchmark configs.
//
// For T=double, there's only a single config. We still use a range arg so
// that its correspondence with the non-double cases is apparent.
//
// For T=AutoDiff, the range arg sets which gradients to use, using a bitmask.
//
// For T=Expression, the range arg sets which variables to use, using a bitmask.

BENCHMARK_DEFINE_F(CassieDouble, MassMatrix)(BenchmarkStateRef state) {
  DoMassMatrix(state);
}
BENCHMARK_REGISTER_F(CassieDouble, MassMatrix)
  ->Unit(benchmark::kMicrosecond)
  ->Arg(kWantNoGrad);

BENCHMARK_DEFINE_F(CassieDouble, InverseDynamics)(BenchmarkStateRef state) {
  DoInverseDynamics(state);
}
BENCHMARK_REGISTER_F(CassieDouble, InverseDynamics)
  ->Unit(benchmark::kMicrosecond)
  ->Arg(kWantNoGrad);

BENCHMARK_DEFINE_F(CassieDouble, ForwardDynamics)(BenchmarkStateRef state) {
  DoForwardDynamics(state);
}
BENCHMARK_REGISTER_F(CassieDouble, ForwardDynamics)
  ->Unit(benchmark::kMicrosecond)
  ->Arg(kWantNoGrad);

BENCHMARK_DEFINE_F(CassieAutoDiff, MassMatrix)(BenchmarkStateRef state) {
  DoMassMatrix(state);
}
BENCHMARK_REGISTER_F(CassieAutoDiff, MassMatrix)
  ->Unit(benchmark::kMicrosecond)
  ->Arg(kWantNoGrad)
  ->Arg(kWantGradQ)
  ->Arg(kWantGradV)
  ->Arg(kWantGradX);

BENCHMARK_DEFINE_F(CassieAutoDiff, InverseDynamics)(BenchmarkStateRef state) {
  DoInverseDynamics(state);
}
BENCHMARK_REGISTER_F(CassieAutoDiff, InverseDynamics)
  ->Unit(benchmark::kMicrosecond)
  ->Arg(kWantNoGrad)
  ->Arg(kWantGradQ)
  ->Arg(kWantGradV)
  ->Arg(kWantGradX)
  ->Arg(kWantGradVdot)
  ->Arg(kWantGradQ|kWantGradVdot)
  ->Arg(kWantGradV|kWantGradVdot)
  ->Arg(kWantGradX|kWantGradVdot);

BENCHMARK_DEFINE_F(CassieAutoDiff, ForwardDynamics)(BenchmarkStateRef state) {
  DoForwardDynamics(state);
}
BENCHMARK_REGISTER_F(CassieAutoDiff, ForwardDynamics)
  ->Unit(benchmark::kMicrosecond)
  ->Arg(kWantNoGrad)
  ->Arg(kWantGradQ)
  ->Arg(kWantGradV)
  ->Arg(kWantGradX)
  ->Arg(kWantGradU)
  ->Arg(kWantGradQ|kWantGradU)
  ->Arg(kWantGradV|kWantGradU)
  ->Arg(kWantGradX|kWantGradU);

BENCHMARK_DEFINE_F(CassieExpression, MassMatrix)(BenchmarkStateRef state) {
  DoMassMatrix(state);
}
BENCHMARK_REGISTER_F(CassieExpression, MassMatrix)
  ->Unit(benchmark::kMicrosecond)
  ->Arg(kWantNoGrad)
  ->Arg(kWantGradQ)
  ->Arg(kWantGradV)
  ->Arg(kWantGradX);

BENCHMARK_DEFINE_F(CassieExpression, InverseDynamics)(BenchmarkStateRef state) {
  DoInverseDynamics(state);
}
BENCHMARK_REGISTER_F(CassieExpression, InverseDynamics)
  ->Unit(benchmark::kMicrosecond)
  ->Arg(kWantNoGrad)
  ->Arg(kWantGradQ)
  ->Arg(kWantGradV)
  ->Arg(kWantGradX)
  ->Arg(kWantGradVdot)
  ->Arg(kWantGradQ|kWantGradVdot)
  ->Arg(kWantGradV|kWantGradVdot)
  ->Arg(kWantGradX|kWantGradVdot);

BENCHMARK_DEFINE_F(CassieExpression, ForwardDynamics)(BenchmarkStateRef state) {
  DoForwardDynamics(state);
}
BENCHMARK_REGISTER_F(CassieExpression, ForwardDynamics)
  ->Unit(benchmark::kMicrosecond)
  ->Arg(kWantNoGrad)
  // N.B. MbP does not support forward dynamics with Variables in 'q'.
  ->Arg(kWantGradV)
  ->Arg(kWantGradU)
  ->Arg(kWantGradV|kWantGradU);

}  // namespace
}  // namespace multibody
}  // namespace drake

BENCHMARK_MAIN();
