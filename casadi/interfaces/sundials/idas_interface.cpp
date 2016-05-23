/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "idas_interface.hpp"
#include "casadi/core/std_vector_tools.hpp"

// Macro for error handling
#define THROWING(fcn, ...) \
idas_error(CASADI_ASSERT_STR(fcn) CASADI_ASSERT_WHERE, fcn(__VA_ARGS__))

using namespace std;
namespace casadi {

  extern "C"
  int CASADI_INTEGRATOR_IDAS_EXPORT
      casadi_register_integrator_idas(Integrator::Plugin* plugin) {
    plugin->creator = IdasInterface::creator;
    plugin->name = "idas";
    plugin->doc = IdasInterface::meta_doc.c_str();
    plugin->version = 30;
    return 0;
  }

  extern "C"
  void CASADI_INTEGRATOR_IDAS_EXPORT casadi_load_integrator_idas() {
    Integrator::registerPlugin(casadi_register_integrator_idas);
  }

  IdasInterface::IdasInterface(const std::string& name, const Function& dae)
    : SundialsInterface(name, dae) {
  }

  IdasInterface::~IdasInterface() {
    clear_memory();
  }

  Options IdasInterface::options_
  = {{&SundialsInterface::options_},
     {{"suppress_algebraic",
       {OT_BOOL,
        "Suppress algebraic variables in the error testing"}},
      {"calc_ic",
       {OT_BOOL,
        "Use IDACalcIC to get consistent initial conditions."}},
      {"calc_icB",
       {OT_BOOL,
        "Use IDACalcIC to get consistent initial conditions for "
        "backwards system [default: equal to calc_ic]."}},
      {"abstolv",
       {OT_DOUBLEVECTOR,
        "Absolute tolerarance for each component"}},
      {"fsens_abstolv",
       {OT_DOUBLEVECTOR,
        "Absolute tolerarance for each component, forward sensitivities"}},
      {"max_step_size",
       {OT_DOUBLE,
        "Maximim step size"}},
      {"first_time",
       {OT_DOUBLE,
        "First requested time as a fraction of the time interval"}},
      {"cj_scaling",
       {OT_BOOL,
        "IDAS scaling on cj for the user-defined linear solver module"}},
      {"extra_fsens_calc_ic",
       {OT_BOOL,
        "Call calc ic an extra time, with fsens=0"}},
      {"init_xdot",
       {OT_DOUBLEVECTOR,
        "Initial values for the state derivatives"}}
     }
  };

  void IdasInterface::init(const Dict& opts) {
    log("IdasInterface::init", "begin");

    // Call the base class init
    SundialsInterface::init(opts);

    // Default options
    cj_scaling_ = false;
    calc_ic_ = true;
    suppress_algebraic_ = false;
    max_step_size_ = 0;

    // Read options
    for (auto&& op : opts) {
      if (op.first=="init_xdot") {
        init_xdot_ = op.second;
      } else if (op.first=="cj_scaling") {
        cj_scaling_ = op.second;
      } else if (op.first=="calc_ic") {
        calc_ic_ = op.second;
      } else if (op.first=="suppress_algebraic") {
        suppress_algebraic_ = op.second;
      } else if (op.first=="max_step_size") {
        max_step_size_ = op.second;
      } else if (op.first=="abstolv") {
        abstolv_ = op.second;
      } else if (op.first=="fsens_abstolv") {
        fsens_abstolv_ = op.second;
      }
    }

    // Default dependent options
    calc_icB_ = calc_ic_;
    first_time_ = grid_.back();

    // Read dependent options
    for (auto&& op : opts) {
      if (op.first=="calc_icB") {
        calc_icB_ = op.second;
      } else if (op.first=="first_time") {
        first_time_ = op.second;
      }
    }

    create_function("daeF", {"x", "z", "p", "t"}, {"ode", "alg"});
    create_function("quadF", {"x", "z", "p", "t"}, {"quad"});
    create_function("daeB", {"rx", "rz", "rp", "x", "z", "p", "t"}, {"rode", "ralg"});
    create_function("quadB", {"rx", "rz", "rp", "x", "z", "p", "t"}, {"rquad"});

    // Create a Jacobian if requested
    set_function(oracle_.is_a("sxfunction") ? getJacF<SX>() : getJacF<MX>());
    init_jacF();
    // Create a backwards Jacobian if requested
    if (nrx_>0) {
      set_function(oracle_.is_a("sxfunction") ? getJacB<SX>() : getJacB<MX>());
      init_jacB();
    }

    // Get initial conditions for the state derivatives
    if (init_xdot_.empty()) {
      init_xdot_.resize(nx_, 0);
    } else {
      casadi_assert_message(
        init_xdot_.size()==nx_,
        "Option \"init_xdot\" has incorrect length. Expecting " << nx_
        << ", but got " << init_xdot_.size()
        << ". Note that this message may actually be generated by the augmented"
        " integrator. In that case, make use of the 'augmented_options' options"
        " to correct 'init_xdot' for the augmented integrator.");
    }

    // Attach functions for jacobian information
    if (iterative_) {
      create_function("jtimesF",
        {"t", "x", "z", "p", "fwd:x", "fwd:z"},
        {"fwd:ode", "fwd:alg"});
      if (nrx_>0) {
        create_function("jtimesB",
          {"t", "x", "z", "p", "rx", "rz", "rp", "fwd:rx", "fwd:rz"},
          {"fwd:rode", "fwd:ralg"});
      }
    }

    log("IdasInterface::init", "end");
  }

  int IdasInterface::res(double t, N_Vector xz, N_Vector xzdot,
                                N_Vector rr, void *user_data) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = NV_DATA_S(xz);
      m->arg[1] = NV_DATA_S(xz)+s.nx_;
      m->arg[2] = m->p;
      m->arg[3] = &t;
      m->res[0] = NV_DATA_S(rr);
      m->res[1] = NV_DATA_S(rr)+s.nx_;
      s.calc_function(m, "daeF");

      // Subtract state derivative to get residual
      casadi_axpy(s.nx_, -1., NV_DATA_S(xzdot), NV_DATA_S(rr));
      return 0;
    } catch(int flag) { // recoverable error
      return flag;
    } catch(exception& e) { // non-recoverable error
      userOut<true, PL_WARN>() << "res failed: " << e.what() << endl;
      return -1;
    }
  }

  void IdasInterface::ehfun(int error_code, const char *module, const char *function,
                                   char *msg, void *eh_data) {
    try {
      //auto m = to_mem(eh_data);
      //auto& s = m->self;
      userOut<true, PL_WARN>() << msg << endl;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "ehfun failed: " << e.what() << endl;
    }
  }

  int IdasInterface::jtimes(double t, N_Vector xz, N_Vector xzdot, N_Vector rr, N_Vector v,
                                   N_Vector Jv, double cj, void *user_data,
                                   N_Vector tmp1, N_Vector tmp2) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(xz);
      m->arg[2] = NV_DATA_S(xz)+s.nx_;
      m->arg[3] = m->p;
      m->arg[4] = NV_DATA_S(v);
      m->arg[5] = NV_DATA_S(v)+s.nx_;
      m->res[0] = NV_DATA_S(Jv);
      m->res[1] = NV_DATA_S(Jv)+s.nx_;
      s.calc_function(m, "jtimesF");

      // Subtract state derivative to get residual
      casadi_axpy(s.nx_, -cj, NV_DATA_S(v), NV_DATA_S(Jv));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "jtimes failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::jtimesB(double t, N_Vector xz, N_Vector xzdot, N_Vector xzB,
                                    N_Vector xzdotB, N_Vector resvalB, N_Vector vB, N_Vector JvB,
                                    double cjB, void *user_data,
                                    N_Vector tmp1B, N_Vector tmp2B) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(xz);
      m->arg[2] = NV_DATA_S(xz)+s.nx_;
      m->arg[3] = m->p;
      m->arg[4] = NV_DATA_S(xzB);
      m->arg[5] = NV_DATA_S(xzB)+s.nrx_;
      m->arg[6] = m->rp;
      m->arg[7] = NV_DATA_S(vB);
      m->arg[8] = NV_DATA_S(vB)+s.nrx_;
      m->res[0] = NV_DATA_S(JvB);
      m->res[1] = NV_DATA_S(JvB) + s.nrx_;
      s.calc_function(m, "jtimesB");

      // Subtract state derivative to get residual
      casadi_axpy(s.nrx_, cjB, NV_DATA_S(vB), NV_DATA_S(JvB));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "jtimesB failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::init_memory(void* mem) const {
    SundialsInterface::init_memory(mem);
    auto m = to_mem(mem);

    // Create IDAS memory block
    m->mem = IDACreate();
    casadi_assert_message(m->mem!=0, "IDACreate: Creation failed");

    // Set error handler function
    THROWING(IDASetErrHandlerFn, m->mem, ehfun, m);

    // Set user data
    THROWING(IDASetUserData, m->mem, m);

    // Allocate n-vectors for ivp
    m->xzdot = N_VNew_Serial(nx_+nz_);

    // Initialize Idas
    double t0 = 0;
    N_VConst(0.0, m->xz);
    N_VConst(0.0, m->xzdot);
    IDAInit(m->mem, res, t0, m->xz, m->xzdot);
    log("IdasInterface::init", "IDA initialized");

    // Include algebraic variables in error testing
    THROWING(IDASetSuppressAlg, m->mem, suppress_algebraic_);

    // Maxinum order for the multistep method
    THROWING(IDASetMaxOrd, m->mem, max_multistep_order_);

    // Set maximum step size
    THROWING(IDASetMaxStep, m->mem, max_step_size_);

    if (!abstolv_.empty()) {
      // Vector absolute tolerances
      N_Vector nv_abstol = N_VNew_Serial(abstolv_.size());
      copy(abstolv_.begin(), abstolv_.end(), NV_DATA_S(nv_abstol));
      THROWING(IDASVtolerances, m->mem, reltol_, nv_abstol);
      N_VDestroy_Serial(nv_abstol);
    } else {
      // Scalar absolute tolerances
      THROWING(IDASStolerances, m->mem, reltol_, abstol_);
    }

    // Maximum number of steps
    THROWING(IDASetMaxNumSteps, m->mem, max_num_steps_);

    // Set algebraic components
    N_Vector id = N_VNew_Serial(nx_+nz_);
    fill_n(NV_DATA_S(id), nx_, 1);
    fill_n(NV_DATA_S(id)+nx_, nz_, 0);

    // Pass this information to IDAS
    THROWING(IDASetId, m->mem, id);

    // Delete the allocated memory
    N_VDestroy_Serial(id);

    // attach a linear solver
    if (iterative_) {
      switch (itsol_) {
      case SD_GMRES: THROWING(IDASpgmr, m->mem, max_krylov_); break;
      case SD_BCGSTAB: THROWING(IDASpbcg, m->mem, max_krylov_); break;
      case SD_TFQMR: THROWING(IDASptfqmr, m->mem, max_krylov_); break;
      }
      THROWING(IDASpilsSetJacTimesVecFn, m->mem, jtimes);
      if (use_precon_) THROWING(IDASpilsSetPreconditioner, m->mem, psetup, psolve);
    } else {
      IDAMem IDA_mem = IDAMem(m->mem);
      IDA_mem->ida_lmem   = m;
      IDA_mem->ida_lsetup = lsetup;
      IDA_mem->ida_lsolve = lsolve;
      IDA_mem->ida_setupNonNull = TRUE;
    }

    // Quadrature equations
    if (nq_>0) {

      // Initialize quadratures in IDAS
      THROWING(IDAQuadInit, m->mem, rhsQ, m->q);

      // Should the quadrature errors be used for step size control?
      if (quad_err_con_) {
        THROWING(IDASetQuadErrCon, m->mem, true);

        // Quadrature error tolerances
        // TODO(Joel): vector absolute tolerances
        THROWING(IDAQuadSStolerances, m->mem, reltol_, abstol_);
      }
    }

    log("IdasInterface::init", "attached linear solver");

    // Adjoint sensitivity problem
    if (nrx_>0) {
      m->rxzdot = N_VNew_Serial(nrx_+nrz_);
      N_VConst(0.0, m->rxz);
      N_VConst(0.0, m->rxzdot);
    }
    log("IdasInterface::init", "initialized adjoint sensitivities");

    // Initialize adjoint sensitivities
    if (nrx_>0) {
      int interpType = interp_==SD_HERMITE ? IDA_HERMITE : IDA_POLYNOMIAL;
      THROWING(IDAAdjInit, m->mem, steps_per_checkpoint_, interpType);
    }

    m->first_callB = true;
  }

  void IdasInterface::reset(IntegratorMemory* mem, double t, const double* _x,
                            const double* _z, const double* _p) const {
    log("IdasInterface::reset", "begin");
    auto m = to_mem(mem);

    // Reset the base classes
    SundialsInterface::reset(mem, t, _x, _z, _p);

    // Re-initialize
    copy(init_xdot_.begin(), init_xdot_.end(), NV_DATA_S(m->xzdot));
    THROWING(IDAReInit, m->mem, grid_.front(), m->xz, m->xzdot);

    // Re-initialize quadratures
    if (nq_>0) THROWING(IDAQuadReInit, m->mem, m->q);

    // Correct initial conditions, if necessary
    if (calc_ic_) {
      THROWING(IDACalcIC, m->mem, IDA_YA_YDP_INIT , first_time_);
      THROWING(IDAGetConsistentIC, m->mem, m->xz, m->xzdot);
    }

    // Re-initialize backward integration
    if (nrx_>0) THROWING(IDAAdjReInit, m->mem);

    // Set the stop time of the integration -- don't integrate past this point
    if (stop_at_end_) setStopTime(m, grid_.back());

    log("IdasInterface::reset", "end");
  }

  void IdasInterface::
  advance(IntegratorMemory* mem, double t, double* x, double* z, double* q) const {
    auto m = to_mem(mem);

    casadi_assert_message(t>=grid_.front(), "IdasInterface::integrate(" << t << "): "
                          "Cannot integrate to a time earlier than t0 (" << grid_.front() << ")");
    casadi_assert_message(t<=grid_.back() || !stop_at_end_, "IdasInterface::integrate("
                          << t << "): "
                          "Cannot integrate past a time later than tf (" << grid_.back() << ") "
                          "unless stop_at_end is set to False.");

    // Integrate, unless already at desired time
    double ttol = 1e-9;   // tolerance
    if (fabs(m->t-t)>=ttol) {
      // Integrate forward ...
      if (nrx_>0) { // ... with taping
        THROWING(IDASolveF, m->mem, t, &m->t, m->xz, m->xzdot, IDA_NORMAL, &m->ncheck);
      } else { // ... without taping
        THROWING(IDASolve, m->mem, t, &m->t, m->xz, m->xzdot, IDA_NORMAL);
      }

      // Get quadratures
      if (nq_>0) {
        double tret;
        THROWING(IDAGetQuad, m->mem, &tret, m->q);
      }
    }

    // Set function outputs
    casadi_copy(NV_DATA_S(m->xz), nx_, x);
    casadi_copy(NV_DATA_S(m->xz)+nx_, nz_, z);
    casadi_copy(NV_DATA_S(m->q), nq_, q);

    // Get stats
    THROWING(IDAGetIntegratorStats, m->mem, &m->nsteps, &m->nfevals, &m->nlinsetups,
             &m->netfails, &m->qlast, &m->qcur, &m->hinused,
             &m->hlast, &m->hcur, &m->tcur);
  }

  void IdasInterface::resetB(IntegratorMemory* mem, double t, const double* rx,
                             const double* rz, const double* rp) const {
    log("IdasInterface::resetB", "begin");
    auto m = to_mem(mem);

    // Reset the base classes
    SundialsInterface::resetB(mem, t, rx, rz, rp);

    if (m->first_callB) {
      // Create backward problem
      THROWING(IDACreateB, m->mem, &m->whichB);
      THROWING(IDAInitB, m->mem, m->whichB, resB, grid_.back(), m->rxz, m->rxzdot);
      THROWING(IDASStolerancesB, m->mem, m->whichB, reltol_, abstol_);
      THROWING(IDASetUserDataB, m->mem, m->whichB, m);
      THROWING(IDASetMaxNumStepsB, m->mem, m->whichB, max_num_steps_);

      // Set algebraic components
      N_Vector id = N_VNew_Serial(nrx_+nrz_);
      fill_n(NV_DATA_S(id), nrx_, 1);
      fill_n(NV_DATA_S(id)+nrx_, nrz_, 0);
      THROWING(IDASetIdB, m->mem, m->whichB, id);
      N_VDestroy_Serial(id);

      // attach linear solver
      if (iterative_) {
        switch (itsol_) {
        case SD_GMRES: THROWING(IDASpgmrB, m->mem, m->whichB, max_krylov_); break;
        case SD_BCGSTAB: THROWING(IDASpbcgB, m->mem, m->whichB, max_krylov_); break;
        case SD_TFQMR: THROWING(IDASptfqmrB, m->mem, m->whichB, max_krylov_); break;
        }
        THROWING(IDASpilsSetJacTimesVecFnB, m->mem, m->whichB, jtimesB);
        if (use_precon_) THROWING(IDASpilsSetPreconditionerB, m->mem, m->whichB, psetupB, psolveB);
      } else {
        IDAMem IDA_mem = IDAMem(m->mem);
        IDAadjMem IDAADJ_mem = IDA_mem->ida_adj_mem;
        IDABMem IDAB_mem = IDAADJ_mem->IDAB_mem;
        IDAB_mem->ida_lmem   = m;
        IDAB_mem->IDA_mem->ida_lmem = m;
        IDAB_mem->IDA_mem->ida_lsetup = lsetupB;
        IDAB_mem->IDA_mem->ida_lsolve = lsolveB;
        IDAB_mem->IDA_mem->ida_setupNonNull = TRUE;
      }

      // Quadratures for the adjoint problem
      THROWING(IDAQuadInitB, m->mem, m->whichB, rhsQB, m->rq);
      if (quad_err_con_) {
        THROWING(IDASetQuadErrConB, m->mem, m->whichB, true);
        THROWING(IDAQuadSStolerancesB, m->mem, m->whichB, reltol_, abstol_);
      }

      // Mark initialized
      m->first_callB = false;
    } else {
      // Re-initialize
      THROWING(IDAReInitB, m->mem, m->whichB, grid_.back(), m->rxz, m->rxzdot);
      if (nrq_>0) {
        // Workaround (bug in SUNDIALS)
        // THROWING(IDAQuadReInitB, m->mem, m->whichB[dir], m->rq[dir]);
        void* memB = IDAGetAdjIDABmem(m->mem, m->whichB);
        THROWING(IDAQuadReInit, memB, m->rq);
      }
    }

    // Correct initial values for the integration if necessary
    if (calc_icB_) {
      THROWING(IDACalcICB, m->mem, m->whichB, grid_.front(), m->xz, m->xzdot);
      THROWING(IDAGetConsistentICB, m->mem, m->whichB, m->rxz, m->rxzdot);
    }

    log("IdasInterface::resetB", "end");

  }

  void IdasInterface::retreat(IntegratorMemory* mem, double t, double* rx,
                              double* rz, double* rq) const {
    auto m = to_mem(mem);

    // Integrate, unless already at desired time
    if (t<m->t) {
      THROWING(IDASolveB, m->mem, t, IDA_NORMAL);
      THROWING(IDAGetB, m->mem, m->whichB, &m->t, m->rxz, m->rxzdot);
      if (nrq_>0) {
        THROWING(IDAGetQuadB, m->mem, m->whichB, &m->t, m->rq);
      }
    }

    // Save outputs
    casadi_copy(NV_DATA_S(m->rxz), nrx_, rx);
    casadi_copy(NV_DATA_S(m->rxz)+nrx_, nrz_, rz);
    casadi_copy(NV_DATA_S(m->rq), nrq_, rq);

    // Get stats
    IDAMem IDA_mem = IDAMem(m->mem);
    IDAadjMem IDAADJ_mem = IDA_mem->ida_adj_mem;
    IDABMem IDAB_mem = IDAADJ_mem->IDAB_mem;
    THROWING(IDAGetIntegratorStats, IDAB_mem->IDA_mem, &m->nstepsB, &m->nfevalsB,
             &m->nlinsetupsB, &m->netfailsB, &m->qlastB, &m->qcurB, &m->hinusedB,
             &m->hlastB, &m->hcurB, &m->tcurB);
  }

  void IdasInterface::idas_error(const char* module, int flag) {
    // Successfull return or warning
    if (flag>=IDA_SUCCESS) return;
    // Construct error message
    stringstream ss;
    char* flagname = IDAGetReturnFlagName(flag);
    ss << module << " returned \"" << flagname << "\"."
       << " Consult IDAS documentation.";
    free(flagname);
    casadi_error(ss.str());
  }

  int IdasInterface::rhsQ(double t, N_Vector xz, N_Vector xzdot, N_Vector rhsQ,
                                 void *user_data) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = NV_DATA_S(xz);
      m->arg[1] = NV_DATA_S(xz)+s.nx_;
      m->arg[2] = m->p;
      m->arg[3] = &t;
      m->res[0] = NV_DATA_S(rhsQ);
      s.calc_function(m, "quadF");

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "rhsQ failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::resB(double t, N_Vector xz, N_Vector xzdot, N_Vector rxz,
                                 N_Vector rxzdot, N_Vector rr, void *user_data) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = NV_DATA_S(rxz);
      m->arg[1] = NV_DATA_S(rxz)+s.nrx_;
      m->arg[2] = m->rp;
      m->arg[3] = NV_DATA_S(xz);
      m->arg[4] = NV_DATA_S(xz)+s.nx_;
      m->arg[5] = m->p;
      m->arg[6] = &t;
      m->res[0] = NV_DATA_S(rr);
      m->res[1] = NV_DATA_S(rr)+s.nrx_;
      s.calc_function(m, "daeB");

      // Subtract state derivative to get residual
      casadi_axpy(s.nrx_, 1., NV_DATA_S(rxzdot), NV_DATA_S(rr));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "resB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::rhsQB(double t, N_Vector xz, N_Vector xzdot, N_Vector rxz,
                                  N_Vector rxzdot, N_Vector rqdot, void *user_data) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = NV_DATA_S(rxz);
      m->arg[1] = NV_DATA_S(rxz)+s.nrx_;
      m->arg[2] = m->rp;
      m->arg[3] = NV_DATA_S(xz);
      m->arg[4] = NV_DATA_S(xz)+s.nx_;
      m->arg[5] = m->p;
      m->arg[6] = &t;
      m->res[0] = NV_DATA_S(rqdot);
      s.calc_function(m, "quadB");

      // Negate (note definition of g)
      casadi_scal(s.nrq_, -1., NV_DATA_S(rqdot));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "rhsQB failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::setStopTime(IntegratorMemory* mem, double tf) const {
    // Set the stop time of the integration -- don't integrate past this point
    auto m = to_mem(mem);
    auto& s = m->self;
    THROWING(IDASetStopTime, m->mem, tf);
  }

  int IdasInterface::psolve(double t, N_Vector xz, N_Vector xzdot, N_Vector rr,
                                    N_Vector rvec, N_Vector zvec, double cj, double delta,
                                    void *user_data, N_Vector tmp) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      // Copy input to output, if necessary
      if (rvec!=zvec) {
        N_VScale(1.0, rvec, zvec);
      }

      // Solve the (possibly factorized) system
      const Function& linsol = s.get_function("linsolF");
      linsol.linsol_solve(NV_DATA_S(zvec));
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psolve failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::psolveB(double t, N_Vector xz, N_Vector xzdot, N_Vector xzB,
                                    N_Vector xzdotB, N_Vector resvalB, N_Vector rvecB,
                                    N_Vector zvecB, double cjB, double deltaB,
                                    void *user_data, N_Vector tmpB) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      // Copy input to output, if necessary
      if (rvecB!=zvecB) {
        N_VScale(1.0, rvecB, zvecB);
      }

      const Function& linsolB = s.get_function("linsolB");
      linsolB.linsol_solve(NV_DATA_S(zvecB));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psolveB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::psetup(double t, N_Vector xz, N_Vector xzdot, N_Vector rr,
                                   double cj, void* user_data,
                                   N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(xz);
      m->arg[2] = NV_DATA_S(xz)+s.nx_;
      m->arg[3] = m->p;
      m->arg[4] = &cj;
      m->res[0] = m->jac;
      s.calc_function(m, "jacF");

      // Prepare the solution of the linear system (e.g. factorize)
      const Function& linsol = s.get_function("linsolF");
      linsol.setup(m->arg+LINSOL_NUM_IN, m->res+LINSOL_NUM_OUT, m->iw, m->w);
      linsol.linsol_factorize(m->jac);

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psetup failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::psetupB(double t, N_Vector xz, N_Vector xzdot,
                                    N_Vector rxz, N_Vector rxzdot,
                                    N_Vector rresval, double cj, void *user_data,
                                    N_Vector tmp1B, N_Vector tmp2B, N_Vector tmp3B) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(rxz);
      m->arg[2] = NV_DATA_S(rxz)+s.nrx_;
      m->arg[3] = m->rp;
      m->arg[4] = NV_DATA_S(xz);
      m->arg[5] = NV_DATA_S(xz)+s.nx_;
      m->arg[6] = m->p;
      m->arg[7] = &cj;
      m->res[0] = m->jacB;
      s.calc_function(m, "jacB");

      // Prepare the solution of the linear system (e.g. factorize)
      const Function& linsolB = s.get_function("linsolB");
      linsolB.setup(m->arg+LINSOL_NUM_IN, m->res+LINSOL_NUM_OUT, m->iw, m->w);
      linsolB.linsol_factorize(m->jacB);

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psetupB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::lsetup(IDAMem IDA_mem, N_Vector xz, N_Vector xzdot, N_Vector resp,
                                    N_Vector vtemp1, N_Vector vtemp2, N_Vector vtemp3) {
    // Current time
    double t = IDA_mem->ida_tn;

    // Multiple of df_dydot to be added to the matrix
    double cj = IDA_mem->ida_cj;

    // Call the preconditioner setup function (which sets up the linear solver)
    if (psetup(t, xz, xzdot, 0, cj, IDA_mem->ida_lmem,
      vtemp1, vtemp1, vtemp3)) return 1;

    return 0;
  }

  int IdasInterface::lsetupB(IDAMem IDA_mem, N_Vector xzB, N_Vector xzdotB, N_Vector respB,
                                     N_Vector vtemp1B, N_Vector vtemp2B, N_Vector vtemp3B) {
    try {
      auto m = to_mem(IDA_mem->ida_lmem);
      //auto& s = m->self;
      IDAadjMem IDAADJ_mem;
      //IDABMem IDAB_mem;

      // Current time
      double t = IDA_mem->ida_tn; // TODO(Joel): is this correct?
      // Multiple of df_dydot to be added to the matrix
      double cj = IDA_mem->ida_cj;

      IDA_mem = static_cast<IDAMem>(IDA_mem->ida_user_data);

      IDAADJ_mem = IDA_mem->ida_adj_mem;
      //IDAB_mem = IDAADJ_mem->ia_bckpbCrt;

      // Get FORWARD solution from interpolation.
      if (IDAADJ_mem->ia_noInterp==FALSE) {
        int flag = IDAADJ_mem->ia_getY(IDA_mem, t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp,
                                   NULL, NULL);
        if (flag != IDA_SUCCESS) casadi_error("Could not interpolate forward states");
      }
      // Call the preconditioner setup function (which sets up the linear solver)
      if (psetupB(t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp,
        xzB, xzdotB, 0, cj, static_cast<void*>(m), vtemp1B, vtemp1B, vtemp3B)) return 1;

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsetupB failed: " << e.what() << endl;
      return -1;
    }
  }

  int IdasInterface::lsolve(IDAMem IDA_mem, N_Vector b, N_Vector weight, N_Vector xz,
                                   N_Vector xzdot, N_Vector rr) {
    try {
      auto m = to_mem(IDA_mem->ida_lmem);
      auto& s = m->self;

      // Current time
      double t = IDA_mem->ida_tn;

      // Multiple of df_dydot to be added to the matrix
      double cj = IDA_mem->ida_cj;

      // Accuracy
      double delta = 0.0;

      // Call the preconditioner solve function (which solves the linear system)
      if (psolve(t, xz, xzdot, rr, b, b, cj,
        delta, static_cast<void*>(m), 0)) return 1;

      // Scale the correction to account for change in cj
      if (s.cj_scaling_) {
        double cjratio = IDA_mem->ida_cjratio;
        if (cjratio != 1.0) N_VScale(2.0/(1.0 + cjratio), b, b);
      }

      return 0;
    } catch(int wrn) {
      /*    userOut<true, PL_WARN>() << "warning: " << wrn << endl;*/
      return wrn;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsolve failed: " << e.what() << endl;
      return -1;
    }
  }

  int IdasInterface::lsolveB(IDAMem IDA_mem, N_Vector b, N_Vector weight, N_Vector xzB,
                                    N_Vector xzdotB, N_Vector rrB) {
    try {
      auto m = to_mem(IDA_mem->ida_lmem);
      auto& s = m->self;
      IDAadjMem IDAADJ_mem;
      //IDABMem IDAB_mem;
      int flag;

      // Current time
      double t = IDA_mem->ida_tn; // TODO(Joel): is this correct?
      // Multiple of df_dydot to be added to the matrix
      double cj = IDA_mem->ida_cj;
      double cjratio = IDA_mem->ida_cjratio;

      IDA_mem = (IDAMem) IDA_mem->ida_user_data;

      IDAADJ_mem = IDA_mem->ida_adj_mem;
      //IDAB_mem = IDAADJ_mem->ia_bckpbCrt;

      // Get FORWARD solution from interpolation.
      if (IDAADJ_mem->ia_noInterp==FALSE) {
        flag = IDAADJ_mem->ia_getY(IDA_mem, t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp,
                                   NULL, NULL);
        if (flag != IDA_SUCCESS) casadi_error("Could not interpolate forward states");
      }

      // Accuracy
      double delta = 0.0;

      // Call the preconditioner solve function (which solves the linear system)
      if (psolveB(t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp, xzB, xzdotB,
        rrB, b, b, cj, delta, static_cast<void*>(m), 0)) return 1;

      // Scale the correction to account for change in cj
      if (s.cj_scaling_) {
        if (cjratio != 1.0) N_VScale(2.0/(1.0 + cjratio), b, b);
      }
      return 0;
    } catch(int wrn) {
      /*    userOut<true, PL_WARN>() << "warning: " << wrn << endl;*/
      return wrn;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsolveB failed: " << e.what() << endl;
      return -1;
    }
  }

  template<typename MatType>
  Function IdasInterface::getJacF() {
    vector<MatType> a = MatType::get_input(oracle_);
    vector<MatType> r = oracle_(a);

    // Get the Jacobian in the Newton iteration
    MatType cj = MatType::sym("cj");
    MatType jac = MatType::jacobian(r[DE_ODE], a[DE_X]) - cj*MatType::eye(nx_);
    if (nz_>0) {
      jac = horzcat(vertcat(jac,
                            MatType::jacobian(r[DE_ALG], a[DE_X])),
                    vertcat(MatType::jacobian(r[DE_ODE], a[DE_Z]),
                            MatType::jacobian(r[DE_ALG], a[DE_Z])));
    }

    // Remove second order terms (for smooth implementation of #940)
    if (ns_>0 && nz_==0) {
      const Sparsity& sp_new = derivative_of_.get_function("jacF").sparsity_out(0);
      jac = project(jac, diagcat(vector<Sparsity>(1+ns_, sp_new)));
    }

    return Function("jacF", {a[DE_T], a[DE_X], a[DE_Z], a[DE_P], cj},
                    {jac});
  }

  template<typename MatType>
  Function IdasInterface::getJacB() {
    vector<MatType> a = MatType::get_input(oracle_);
    vector<MatType> r = oracle_(a);

    // Get the Jacobian in the Newton iteration
    MatType cj = MatType::sym("cj");
    MatType jac = MatType::jacobian(r[DE_RODE], a[DE_RX]) + cj*MatType::eye(nrx_);
    if (nrz_>0) {
      jac = horzcat(vertcat(jac,
                            MatType::jacobian(r[DE_RALG], a[DE_RX])),
                    vertcat(MatType::jacobian(r[DE_RODE], a[DE_RZ]),
                            MatType::jacobian(r[DE_RALG], a[DE_RZ])));
    }

    // Remove second order terms (for smooth implementation of #940)
    if (ns_>0 && nrz_==0) {
      const Sparsity& sp_new = derivative_of_.get_function("jacB").sparsity_out(0);
      jac = project(jac, diagcat(vector<Sparsity>(1+ns_, sp_new)));
    }

    return Function("jacB", {a[DE_T], a[DE_RX], a[DE_RZ], a[DE_RP],
                             a[DE_X], a[DE_Z], a[DE_P], cj},
                    {jac});
  }

  IdasMemory::IdasMemory(const IdasInterface& s) : self(s) {
    this->mem = 0;
    this->xzdot = 0;
    this->rxzdot = 0;

    // Reset checkpoints counter
    this->ncheck = 0;
  }

  IdasMemory::~IdasMemory() {
    if (this->mem) IDAFree(&this->mem);
    if (this->xzdot) N_VDestroy_Serial(this->xzdot);
    if (this->rxzdot) N_VDestroy_Serial(this->rxzdot);
  }

} // namespace casadi
