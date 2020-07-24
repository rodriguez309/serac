// Copyright (c) 2019, Lawrence Livermore National Security, LLC and
// other Serac Project Developers. See the top-level LICENSE file for
// details.
//
// SPDX-License-Identifier: (BSD-3-Clause)

#include "nonlinear_solid_solver.hpp"

#include "common/logger.hpp"
#include "integrators/hyperelastic_traction_integrator.hpp"
#include "integrators/inc_hyperelastic_integrator.hpp"

const int num_fields = 2;

NonlinearSolidSolver::NonlinearSolidSolver(int order, std::shared_ptr<mfem::ParMesh> pmesh)
    : BaseSolver(pmesh->GetComm(), num_fields),
      m_velocity(m_state[0]),
      m_displacement(m_state[1]),
      m_newton_solver(pmesh->GetComm())
{
  m_velocity->mesh      = pmesh;
  m_velocity->coll      = std::make_shared<mfem::H1_FECollection>(order, pmesh->Dimension());
  m_velocity->space     = std::make_shared<mfem::ParFiniteElementSpace>(pmesh.get(), m_velocity->coll.get(),
                                                                    pmesh->Dimension(), mfem::Ordering::byVDIM);
  m_velocity->gf        = std::make_shared<mfem::ParGridFunction>(m_velocity->space.get());
  *m_velocity->gf       = 0.0;
  m_velocity->true_vec  = std::make_shared<mfem::HypreParVector>(m_velocity->space.get());
  *m_velocity->true_vec = 0.0;
  m_velocity->name      = "velocity";

  m_displacement->mesh      = pmesh;
  m_displacement->coll      = std::make_shared<mfem::H1_FECollection>(order, pmesh->Dimension());
  m_displacement->space     = std::make_shared<mfem::ParFiniteElementSpace>(pmesh.get(), m_displacement->coll.get(),
                                                                        pmesh->Dimension(), mfem::Ordering::byVDIM);
  m_displacement->gf        = std::make_shared<mfem::ParGridFunction>(m_displacement->space.get());
  *m_displacement->gf       = 0.0;
  m_displacement->true_vec  = std::make_shared<mfem::HypreParVector>(m_displacement->space.get());
  *m_displacement->true_vec = 0.0;
  m_displacement->name      = "displacement";

  // Initialize the mesh node pointers
  m_reference_nodes = std::make_unique<mfem::ParGridFunction>(m_displacement->space.get());
  pmesh->GetNodes(*m_reference_nodes);
  pmesh->NewNodes(*m_reference_nodes);

  m_deformed_nodes = std::make_unique<mfem::ParGridFunction>(*m_reference_nodes);

  // Initialize the true DOF vector
  int              true_size = m_velocity->space->TrueVSize();
  mfem::Array<int> true_offset(3);
  true_offset[0] = 0;
  true_offset[1] = true_size;
  true_offset[2] = 2 * true_size;
  m_block        = std::make_unique<mfem::BlockVector>(true_offset);

  m_block->GetBlockView(1, *m_displacement->true_vec);
  *m_displacement->true_vec = 0.0;

  m_block->GetBlockView(0, *m_velocity->true_vec);
  *m_velocity->true_vec = 0.0;
}

void NonlinearSolidSolver::SetDisplacementBCs(const std::set<int> &                 disp_bdr,
                                              std::shared_ptr<mfem::VectorCoefficient> disp_bdr_coef)
{
  SetEssentialBCs(disp_bdr, disp_bdr_coef, *m_displacement->space, -1);
}

void NonlinearSolidSolver::SetDisplacementBCs(const std::set<int> &           disp_bdr,
                                              std::shared_ptr<mfem::Coefficient> disp_bdr_coef, int component)
{
  SetEssentialBCs(disp_bdr, disp_bdr_coef, *m_displacement->space, component);
}

void NonlinearSolidSolver::SetTractionBCs(const std::set<int> &                 trac_bdr,
                                          std::shared_ptr<mfem::VectorCoefficient> trac_bdr_coef, int component)
{
  SetNaturalBCs(trac_bdr, trac_bdr_coef, component);
}

void NonlinearSolidSolver::SetHyperelasticMaterialParameters(double mu, double K)
{
  m_model.reset(new mfem::NeoHookeanModel(mu, K));
}

void NonlinearSolidSolver::SetViscosity(std::shared_ptr<mfem::Coefficient> visc) { m_viscosity = visc; }

void NonlinearSolidSolver::SetDisplacement(mfem::VectorCoefficient &disp_state)
{
  disp_state.SetTime(m_time);
  m_displacement->gf->ProjectCoefficient(disp_state);
  m_gf_initialized[1] = true;
}

void NonlinearSolidSolver::SetVelocity(mfem::VectorCoefficient &velo_state)
{
  velo_state.SetTime(m_time);
  m_velocity->gf->ProjectCoefficient(velo_state);
  m_gf_initialized[0] = true;
}

void NonlinearSolidSolver::SetSolverParameters(const serac::LinearSolverParameters &   lin_params,
                                               const serac::NonlinearSolverParameters &nonlin_params)
{
  m_lin_params    = lin_params;
  m_nonlin_params = nonlin_params;
}

void NonlinearSolidSolver::CompleteSetup()
{
  // Define the nonlinear form
  auto m_H_form = std::make_shared<mfem::ParNonlinearForm>(m_displacement->space.get());

  // Add the hyperelastic integrator
  if (m_timestepper == serac::TimestepMethod::QuasiStatic) {
    m_H_form->AddDomainIntegrator(new IncrementalHyperelasticIntegrator(m_model.get()));
  } else {
    m_H_form->AddDomainIntegrator(new mfem::HyperelasticNLFIntegrator(m_model.get()));
  }

  // Add the traction integrator
  for (auto &nat_bc_data : m_nat_bdr) {
    SLIC_ERROR_IF(!std::holds_alternative<std::shared_ptr<mfem::VectorCoefficient>>(nat_bc_data.coef), 
                  "Traction boundary condition had a non-vector coefficient.");
    m_H_form->AddBdrFaceIntegrator(new HyperelasticTractionIntegrator(
      *std::get<std::shared_ptr<mfem::VectorCoefficient>>(nat_bc_data.coef)), nat_bc_data.markers);
  }

  // Add the essential boundary
  mfem::Array<int> essential_dofs(0);

  // Build the dof array lookup tables
  m_displacement->space->BuildDofToArrays();

  // Project the essential boundary coefficients
  for (const auto &bc : m_ess_bdr) {
    // Generate the scalar dof list from the vector dof list
    mfem::Array<int> dof_list(bc.true_dofs.Size());
    // Use the const version of the BoundaryCondition for correctness
    std::transform(bc.true_dofs.begin(), bc.true_dofs.end(), dof_list.begin(), [&bc = std::as_const(bc), this](const int tdof) {
      auto dof = m_displacement->space->VDofToDof(tdof);
      SLIC_WARNING_IF((bc.component != -1) && (tdof != m_displacement->space->DofToVDof(dof, bc.component)),
                      "Single-component boundary condition tdofs do not match provided component.");
      return dof;
    });

    // Project the coefficient
    if (bc.component == -1) {
      // If it contains all components, project the vector
      SLIC_ERROR_IF(!std::holds_alternative<std::shared_ptr<mfem::VectorCoefficient>>(bc.coef), 
                    "Displacement boundary condition contained all components but had a non-vector coefficient.");
      m_displacement->gf->ProjectCoefficient(*std::get<std::shared_ptr<mfem::VectorCoefficient>>(bc.coef), dof_list);
    } else {
      // If it is only a single component, project the scalar
      SLIC_ERROR_IF(!std::holds_alternative<std::shared_ptr<mfem::Coefficient>>(bc.coef), 
                    "Displacement boundary condition contained a single component but had a non-scalar coefficient.");
      m_displacement->gf->ProjectCoefficient(*std::get<std::shared_ptr<mfem::Coefficient>>(bc.coef), dof_list, bc.component);
    }

    // Add the vector dofs to the total essential BC dof list
    essential_dofs.Append(bc.true_dofs);
  }

  // Remove any duplicates from the essential BC list
  essential_dofs.Sort();
  essential_dofs.Unique();

  m_H_form->SetEssentialTrueDofs(essential_dofs);

  // The abstract mass bilinear form
  std::shared_ptr<mfem::ParBilinearForm> m_M_form;

  // The abstract viscosity bilinear form
  std::shared_ptr<mfem::ParBilinearForm> m_S_form;

  // If dynamic, create the mass and viscosity forms
  if (m_timestepper != serac::TimestepMethod::QuasiStatic) {
    const double              ref_density = 1.0;  // density in the reference configuration
    mfem::ConstantCoefficient rho0(ref_density);

    m_M_form = std::make_shared<mfem::ParBilinearForm>(m_displacement->space.get());

    m_M_form->AddDomainIntegrator(new mfem::VectorMassIntegrator(rho0));
    m_M_form->Assemble(0);
    m_M_form->Finalize(0);

    m_S_form = std::make_shared<mfem::ParBilinearForm>(m_displacement->space.get());
    m_S_form->AddDomainIntegrator(new mfem::VectorDiffusionIntegrator(*m_viscosity));
    m_S_form->Assemble(0);
    m_S_form->Finalize(0);
  }

  // Set up the jacbian solver based on the linear solver options
  std::unique_ptr<mfem::IterativeSolver> iter_solver;

  if (m_lin_params.prec == serac::Preconditioner::BoomerAMG) {
    SLIC_WARNING_IF(m_displacement->space->GetOrdering() == mfem::Ordering::byVDIM,
                    "Attempting to use BoomerAMG with nodal ordering.");
    auto prec_amg = std::make_unique<mfem::HypreBoomerAMG>();
    prec_amg->SetPrintLevel(m_lin_params.print_level);
    prec_amg->SetElasticityOptions(m_displacement->space.get());
    m_J_prec = std::move(prec_amg);

    iter_solver = std::make_unique<mfem::GMRESSolver>(m_displacement->space->GetComm());
  } else {
    auto J_hypreSmoother = std::make_unique<mfem::HypreSmoother>();
    J_hypreSmoother->SetType(mfem::HypreSmoother::l1Jacobi);
    J_hypreSmoother->SetPositiveDiagonal(true);
    m_J_prec = std::move(J_hypreSmoother);

    iter_solver = std::make_unique<mfem::MINRESSolver>(m_displacement->space->GetComm());
  }

  iter_solver->SetRelTol(m_lin_params.rel_tol);
  iter_solver->SetAbsTol(m_lin_params.abs_tol);
  iter_solver->SetMaxIter(m_lin_params.max_iter);
  iter_solver->SetPrintLevel(m_lin_params.print_level);
  iter_solver->SetPreconditioner(*m_J_prec);
  m_J_solver = std::move(iter_solver);

  // Set the newton solve parameters
  m_newton_solver.SetSolver(*m_J_solver);
  m_newton_solver.SetPrintLevel(m_nonlin_params.print_level);
  m_newton_solver.SetRelTol(m_nonlin_params.rel_tol);
  m_newton_solver.SetAbsTol(m_nonlin_params.abs_tol);
  m_newton_solver.SetMaxIter(m_nonlin_params.max_iter);

  // Set the MFEM abstract operators for use with the internal MFEM solvers
  if (m_timestepper == serac::TimestepMethod::QuasiStatic) {
    m_newton_solver.iterative_mode = true;
    m_nonlinear_oper               = std::make_shared<NonlinearSolidQuasiStaticOperator>(m_H_form);
    m_newton_solver.SetOperator(*m_nonlinear_oper);
  } else {
    m_newton_solver.iterative_mode = false;
    m_timedep_oper = std::make_shared<NonlinearSolidDynamicOperator>(m_H_form, m_S_form, m_M_form, m_ess_bdr,
                                                                     m_newton_solver, m_lin_params);
    m_ode_solver->Init(*m_timedep_oper);
  }
}

// Solve the Quasi-static Newton system
void NonlinearSolidSolver::QuasiStaticSolve()
{
  mfem::Vector zero;
  m_newton_solver.Mult(zero, *m_displacement->true_vec);
}

// Advance the timestep
void NonlinearSolidSolver::AdvanceTimestep(double &dt)
{
  // Initialize the true vector
  m_velocity->gf->GetTrueDofs(*m_velocity->true_vec);
  m_displacement->gf->GetTrueDofs(*m_displacement->true_vec);

  // Set the mesh nodes to the reference configuration
  m_displacement->mesh->NewNodes(*m_reference_nodes);
  m_velocity->mesh->NewNodes(*m_reference_nodes);

  if (m_timestepper == serac::TimestepMethod::QuasiStatic) {
    QuasiStaticSolve();
  } else {
    m_ode_solver->Step(*m_block, m_time, dt);
  }

  // Distribute the shared DOFs
  m_velocity->gf->SetFromTrueDofs(*m_velocity->true_vec);
  m_displacement->gf->SetFromTrueDofs(*m_displacement->true_vec);

  // Update the mesh with the new deformed nodes
  m_deformed_nodes->Set(1.0, *m_displacement->gf);

  if (m_timestepper == serac::TimestepMethod::QuasiStatic) {
    m_deformed_nodes->Add(1.0, *m_reference_nodes);
  }

  m_displacement->mesh->NewNodes(*m_deformed_nodes);
  m_velocity->mesh->NewNodes(*m_deformed_nodes);

  m_cycle += 1;
}

NonlinearSolidSolver::~NonlinearSolidSolver() {}
