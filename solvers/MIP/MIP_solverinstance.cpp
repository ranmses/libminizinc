// * -*- mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */

/*
 *  Main authors:
 *     Gleb Belov <gleb.belov@monash.edu>
 */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was ! distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* This is the main file for a mzn-cplex solver using a unified
 * linearization module && a flexible flattener-to-solver interface
 */

#ifdef _MSC_VER 
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>

using namespace std;

#include <minizinc/solvers/MIP/MIP_solverinstance.h>
#include <minizinc/eval_par.hh>

using namespace MiniZinc;

#ifndef __NO_EXPORT_MIP_SOLVERINSTANCE__  // define this to avoid exporting
  MIP_SolverFactory mip_solverfactory;
#endif  // __NO_EXPORT_MIP_SOLVERINSTANCE__

string MIP_SolverFactory::getVersion()
{
  string v = "  MIP solver plugin, compiled  " __DATE__ "  " __TIME__
    ", using: "
    + MIP_WrapperFactory::getVersion();
  return v;
}


MIP_solver::Variable MIP_solverinstance::exprToVar(Expression* arg) {
  MIP_solver::Variable var;
  if (IntLit* il = arg->dyn_cast<IntLit>()) {
    var = mip_wrap->addLitVar(il->v().toInt());
    return var;
  } else if (FloatLit* fl = arg->dyn_cast<FloatLit>()) {
    var = mip_wrap->addLitVar(fl->v());        // can we avoid creating constant variables?  TODO
    return var;
  } else if (BoolLit* bl = arg->dyn_cast<BoolLit>()) {
    var = mip_wrap->addLitVar(bl->v());
    return var;
  } else if (Id* ident = arg->dyn_cast<Id>()) {
//      cerr << "  Looking for variable '" << ident->decl()->id()->str().c_str() <<"'. "
//        "  id == " << (ident) << ",  id->decl() == " << ident->decl() << endl;
//     ofstream ofs("IdMap.txt", ios::app);
//     static int nnn=0;
//     ++ nnn;
//     ofs << "\n\n\n LOOKUP No. " << nnn
//       << "  Looking for variable '" << ident->decl()->id()->str().c_str() <<"'. "
//       "  id == " << (ident) << ",  id->decl() == " << ident->decl() << endl;
//     for (auto ivi=_variableMap.begin(); ivi!=_variableMap.end(); ++ivi)
//       ofs << ivi->first->str().c_str() << " : " << ivi->second << '\n';
//     ofs.close();
//     cerr << "  ... VarId == " << (_variableMap.get(ident->decl()->id())) << endl;
    return _variableMap.get(ident->decl()->id());
  }

  ostringstream oss;
  oss << "unknown expression type: " << arg->eid() << " for arg=" << arg;
  cerr << oss.str() << flush;
  oss << (*arg);
  throw InternalError( oss.str() );
}

void MIP_solverinstance::exprToVarArray(Expression* arg, vector<VarId> &vars) {
  ArrayLit* al = eval_array_lit(getEnv()->envi(), arg);
  vars.clear();
  vars.reserve(al->v().size());
  for (unsigned int i=0; i<al->v().size(); i++)
    vars.push_back(exprToVar(al->v()[i]));
}

void MIP_solverinstance::exprToArray(Expression* arg, vector<double> &vals) {
  ArrayLit* al = eval_array_lit(getEnv()->envi(), arg);
  vals.clear();
  vals.reserve(al->v().size());
  for (unsigned int i=0; i<al->v().size(); i++) {
    if (IntLit* il = al->v()[i]->dyn_cast<IntLit>()) {
      vals.push_back( il->v().toInt() );
    } else if (FloatLit* fl = al->v()[i]->dyn_cast<FloatLit>()) {
      vals.push_back( fl->v() );
    } else if (BoolLit* bl = al->v()[i]->dyn_cast<BoolLit>()) {
      vals.push_back( bl->v() );
    } else {
      throw InternalError("unexpected expression");
    }
  }
}

namespace SCIPConstraints {

  bool CheckAnnUserCut(const Call* call) {
    if(!call->ann().isEmpty()) {
      if(call->ann().contains(constants().ann.user_cut)) {
        return true;
      }
    }
    return false;
  }
  bool CheckAnnLazyConstraint(const Call* call) {
    if(!call->ann().isEmpty()) {
      if(call->ann().contains(constants().ann.lazy_constraint)) {
        return true;
      }
    }
    return false;
  }
  int GetMaskConsType(const Call* call) {
    int mask=0;
      const bool fUC = CheckAnnUserCut(call);
      const bool fLC = CheckAnnLazyConstraint(call);
      if (fUC) {
        mask |= MIP_wrapper::MaskConsType_Usercut;
      }
      if (fLC) {
        mask |= MIP_wrapper::MaskConsType_Lazy;
      }
      if (!fUC && !fLC)
        mask |= MIP_wrapper::MaskConsType_Normal;
      return mask;
//       return MIP_wrapper::MaskConsType_Normal;    // recognition fails
  }

  void p_lin(SolverInstanceBase& si, const Call* call, MIP_wrapper::LinConType lt) {
    MIP_solverinstance& gi = dynamic_cast<MIP_solverinstance&>( si );
    Env& _env = gi.env();
    ASTExprVec<Expression> args = call->args();
    ArrayLit* al = eval_array_lit(_env.envi(), args[0]);
    int nvars = al->v().size();
    vector<double> coefs;
    gi.exprToArray(args[0], coefs);
    vector<MIP_solverinstance::VarId> vars;
    gi.exprToVarArray(args[1], vars);
    assert(coefs.size() == vars.size());
    IntVal ires;
    FloatVal fres;

    double rhs;
    if(args[2]->type().isint()) {
      ires = eval_int(_env.envi(), args[2]);
      rhs = ires.toInt();
    } else if(args[2]->type().isfloat()) {
      fres = eval_float(_env.envi(), args[2]);
      rhs = fres;
    } else {
      throw InternalError("p_lin: rhs unknown type");
    }

    // See if the solver adds indexation itself: no.
    std::stringstream ss;
    ss << "p_lin_" << (gi.getMIPWrapper()->nAddedRows++);
//     cerr << "  coefs: ";
//     for (size_t i=0; i<coefs.size(); ++i)
//       cerr << coefs[i] << ", ";
//     cerr << endl;
    gi.getMIPWrapper()->addRow(nvars, &vars[0], &coefs[0], lt, rhs,
                               GetMaskConsType(call), ss.str());
  }

  void p_int_lin_le(SolverInstanceBase& si, const Call* call) {
    p_lin(si, call, MIP_wrapper::LQ);
  }
  void p_int_lin_eq(SolverInstanceBase& si, const Call* call) {
    p_lin(si, call, MIP_wrapper::EQ);
  }
  void p_float_lin_le(SolverInstanceBase& si, const Call* call) {
    p_lin(si, call, MIP_wrapper::LQ);
  }
  void p_float_lin_eq(SolverInstanceBase& si, const Call* call) {
    p_lin(si, call, MIP_wrapper::EQ);
  }

  // The non-_lin constraints happen in a failed model || in a non-optimized one:
   void p_non_lin(SolverInstanceBase& si, const Call* call, MIP_wrapper::LinConType nCmp) {
      MIP_solverinstance& gi = dynamic_cast<MIP_solverinstance&>( si );
      ASTExprVec<Expression> args = call->args();
      vector<MIP_solver::Variable> vars(2);
      vars[0] = gi.exprToVar(args[0]);
      vars[1] = gi.exprToVar(args[1]);
      double coefs[2] = {1.0, -1.0};

      std::stringstream ss;
      ss << "p_eq_" << (gi.getMIPWrapper()->nAddedRows++);
      gi.getMIPWrapper()->addRow(2, &vars[0], &coefs[0], nCmp, 0.0,
                               GetMaskConsType(call), ss.str());
    }
   void p_eq(SolverInstanceBase& si, const Call* call) {
     p_non_lin( si, call, MIP_wrapper::EQ );
   }
   void p_le(SolverInstanceBase& si, const Call* call) {
     p_non_lin( si, call, MIP_wrapper::LQ );
   }
}

void MIP_solverinstance::registerConstraints() {
  _constraintRegistry.add(ASTString("int2float"),    SCIPConstraints::p_eq);
  _constraintRegistry.add(ASTString("bool_eq"),      SCIPConstraints::p_eq);   // for inconsistency reported in fzn
  _constraintRegistry.add(ASTString("int_eq"),       SCIPConstraints::p_eq);
  _constraintRegistry.add(ASTString("int_le"),       SCIPConstraints::p_le);
  _constraintRegistry.add(ASTString("int_lin_eq"),   SCIPConstraints::p_int_lin_eq);
  _constraintRegistry.add(ASTString("int_lin_le"),   SCIPConstraints::p_int_lin_le);
//   _constraintRegistry.add(ASTString("int_plus"),     SCIPConstraints::p_plus);
//   _constraintRegistry.add(ASTString("bool2int"),     SCIPConstraints::p_eq);
  _constraintRegistry.add(ASTString("float_eq"),     SCIPConstraints::p_eq);
  _constraintRegistry.add(ASTString("float_le"),     SCIPConstraints::p_le);
  _constraintRegistry.add(ASTString("float_lin_eq"), SCIPConstraints::p_float_lin_eq);
  _constraintRegistry.add(ASTString("float_lin_le"), SCIPConstraints::p_float_lin_le);
//   _constraintRegistry.add(ASTString("float_plus"),   SCIPConstraints::p_plus);
}

void MIP_solverinstance::printSolution(ostream& os) {
  assignSolutionToOutput();
  std::stringstream ss;
  getEnv()->evalOutput(ss);
  std::string output = ss.str();

  std::hash<std::string> str_hash;
  size_t h = str_hash(output);
  if(previousOutput.find(h) == previousOutput.end()) {
    previousOutput.insert(h);
    std::cout << output;
    if ( getOptions().getBoolParam("verbose") )
      printStatistics(cout, 1);
    std::cout << "----------" << std::endl;
  }
}

void MIP_solverinstance::printStatistics(ostream& os, bool fLegend)
{
    {
      int nPrec = cout.precision(12);
      cout << "  % MIP Status: " << mip_wrap->getStatusName() << endl;
      if (fLegend)
        cout << "  % obj, bound, CPU_time, nodes (left): ";
      cout << mip_wrap->getObjValue() << ",  ";
      cout << mip_wrap->getBestBound() << ",  ";
      cout << mip_wrap->getCPUTime() << ",  ";
      cout << mip_wrap->getNNodes();
      if (mip_wrap->getNOpen())
        cout << " ( " << mip_wrap->getNOpen() << " )";
      cout << endl;
      cout.precision(nPrec);
//       std::cout << "% MIP_Objective_ : " << mip_wrap->getObjValue() << std::endl;
// //         std::cout << "% MIP_AbsGap__   : "
// //           << std::fabs(_ilocplex->getBestObjValue()-_ilocplex->getObjValue()) << std::endl;
// //         std::cout << "% MIP_RelGap__   : " << _ilocplex->getMIPRelativeGap() << std::endl;
//       std::cout   << "% MIP_BestBound_ : " << mip_wrap->getBestBound() << std::endl;
//       std::cout   << "% Real/CPU Time_ : " << mip_wrap->getCPUTime() << " sec\n" << std::endl;
//       std::cout << "%------------------------------------------------------------------------\n"<< std::endl;
    }
}


void HandleSolutionCallback(const MIP_wrapper::Output& out, void* pp) {
  // multi-threading? TODO
  MIP_solverinstance* pSI = (MIP_solverinstance*)( pp );
  assert(pSI);
  if (fabs(pSI->lastIncumbent - out.objVal) > 1e-12*(1.0 + fabs(out.objVal))) {
    pSI->lastIncumbent = out.objVal;
    pSI->printSolution(std::cout);
  }
}


SolverInstance::Status MIP_solverinstance::solve(void) {
  SolveI* solveItem = getEnv()->flat()->solveItem();
  if (solveItem->st() != SolveI::SolveType::ST_SAT) {
    if (solveItem->st() == SolveI::SolveType::ST_MAX) {
      getMIPWrapper()->setObjSense(1);
      if (mip_wrap->fVerbose)
        cerr << "    MIP_solverinstance: this is a MAXimization problem." << endl;
    } else {
      getMIPWrapper()->setObjSense(-1);
      if (mip_wrap->fVerbose)
        cerr << "    MIP_solverinstance: this is a MINimization problem." << endl;
    }
    if (mip_wrap->fVerbose) {
      cerr << "    MIP_solverinstance: bounds for the objective function: "
        << dObjVarLB << ", " << dObjVarUB << endl;
    }
  } else {
    if (mip_wrap->fVerbose)
      cerr << "    MIP_solverinstance: this is a SATisfiability problem." << endl;
  }
  
  
  lastIncumbent = 1e200;                  // for callbacks
  getMIPWrapper()->provideSolutionCallback(HandleSolutionCallback, this);

  getMIPWrapper()->solve();
  
//   printStatistics(cout, 1);   MznSolver does this (if it wants)

  MIP_wrapper::Status sw = getMIPWrapper()->getStatus();
  SolverInstance::Status s = SolverInstance::UNKNOWN;
  switch(sw) {
    case MIP_wrapper::Status::OPT:
      if ( SolveI::SolveType::ST_SAT != getEnv()->flat()->solveItem()->st() ) {
        s = SolverInstance::OPT;
        break;
      }    // else: for SAT problems just say SAT
    case MIP_wrapper::Status::SAT:
      s = SolverInstance::SAT;
      break;
    case MIP_wrapper::Status::UNSAT:
      s = SolverInstance::UNSAT;
      break;
    case MIP_wrapper::Status::UNBND:
      s = SolverInstance::UNBND;
      break;
    case MIP_wrapper::Status::UNSATorUNBND:
      s = SolverInstance::UNSATorUNBND;
      break;
    case MIP_wrapper::Status::UNKNOWN:
      s = SolverInstance::UNKNOWN;
      break;
    default:
      s = SolverInstance::ERROR;
  }
  return s;
}

void MIP_solverinstance::processFlatZinc(void) {
  /// last-minute solver params
  mip_wrap->fVerbose = (getOptions().getBoolParam("verbose"));

  SolveI* solveItem = getEnv()->flat()->solveItem();
  VarDecl* objVd = NULL;

  if (solveItem->st() != SolveI::SolveType::ST_SAT) {
    if(Id* id = solveItem->e()->dyn_cast<Id>()) {
      objVd = id->decl();
    } else {
      std::cerr << "Objective must be Id: " << solveItem->e() << std::endl;
      throw InternalError("Objective must be Id");
    }
  }

  for (VarDeclIterator it = getEnv()->flat()->begin_vardecls(); it != getEnv()->flat()->end_vardecls(); ++it) {
    VarDecl* vd = it->e();
    if(!vd->ann().isEmpty()) {
      if(vd->ann().containsCall(constants().ann.output_array.aststr()) ||
          vd->ann().contains(constants().ann.output_var)
        ) {
        _varsWithOutput.push_back(vd);
      }
    }
    if (vd->type().dim() == 0 && it->e()->type().isvar() && !it->removed()) {
      MiniZinc::TypeInst* ti = it->e()->ti();
      MIP_wrapper::VarType vType = MIP_wrapper::VarType::REAL;     // fInt = false;
      if (ti->type().isvarint() || ti->type().isint())
        vType = MIP_wrapper::VarType::INT;
      else if (ti->type().isvarbool() || ti->type().isbool()) {
        vType = MIP_wrapper::VarType::BINARY;
      } else if (ti->type().isvarfloat() || ti->type().isfloat()) {
      } else {
        std::stringstream ssm;
        ssm << "This type of var is ! handled by MIP: " << *it << std::endl;
        ssm << "  VarDecl flags (ti, bt, st, ot): "
          << ti->type().ti()
          << ti->type().bt()
          << ti->type().st()
          << ti->type().ot()
          << ", dim == " << ti->type().dim()
          << endl;
        throw InternalError(ssm.str());
      }
      double lb=0.0, ub=1.0;  // for bool
      if (ti->domain()) {
        if (MIP_wrapper::VarType::REAL == vType) {
          FloatBounds fb = compute_float_bounds(getEnv()->envi(), it->e()->id());
          assert(fb.valid);
          lb = fb.l;
          ub = fb.u;
        } else if (MIP_wrapper::VarType::INT == vType) {
          IntBounds ib = compute_int_bounds(getEnv()->envi(), it->e()->id());
          assert(ib.valid);
          lb = ib.l.toInt();
          ub = ib.u.toInt();
        } 
      } else if (MIP_wrapper::VarType::BINARY != vType) {
        lb = -getMIPWrapper()->getInfBound();  // if just 1 bound inf, using MZN's default?  TODO
        ub = -lb;
      }

//       IntSetVal* dom = eval_intset(env,vdi->e()->ti()->domain());
//       if (dom->size() > 1)
//         throw runtime_error("MIP_solverinstance: domains with holes ! supported, use --MIPdomains");

      VarId res;
      Id* id = it->e()->id();
      id = id->decl()->id();
      if (it->e()->e()) {
        res = exprToVar(it->e()->e());     // modify obj coef??     TODO
      } else {
        double obj = vd==objVd ? 1.0 : 0.0;
        res = getMIPWrapper()->addVar(obj, lb, ub, vType, id->str().c_str());
        if (vd==objVd) {
          dObjVarLB = lb;
          dObjVarUB = ub;
        }
      }
//       if ("X_INTRODUCED_108" == string(id->str().c_str()))
//        std::cerr << "  VarMap: Inserting '" << id->str().c_str() << "' as " << res
//            << ", id == " << (id) << ", id->decl() == " << (id->decl()) << endl;
      _variableMap.insert(id, res);
      assert( res == _variableMap.get(id) );
    }
  }
  if (mip_wrap->fVerbose && mip_wrap->sLitValues.size())
    cerr << "  MIP_solverinstance: during Phase 1,  "
      << mip_wrap->nLitVars << " literals with "
      << mip_wrap-> sLitValues.size() << " values used." << endl;
  if (! getMIPWrapper()->fPhase1Over)
    getMIPWrapper()->addPhase1Vars(); 

  if (mip_wrap->fVerbose)
    cerr << "  MIP_solverinstance: adding constraints..." << flush;
  
  for (ConstraintIterator it = getEnv()->flat()->begin_constraints(); it != getEnv()->flat()->end_constraints(); ++it) {
    if (Call* c = it->e()->dyn_cast<Call>()) {
      _constraintRegistry.post(c);
    }
  }
  if (mip_wrap->fVerbose)
    cerr << " done, " << mip_wrap->getNRows() << " rows && "
    << mip_wrap->getNCols() << " columns in total." << endl;
  if (mip_wrap->fVerbose && mip_wrap->sLitValues.size())
    cerr << "  MIP_solverinstance: overall,  "
      << mip_wrap->nLitVars << " literals with "
      << mip_wrap-> sLitValues.size() << " values used." << endl;
}  // processFlatZinc

Expression* MIP_solverinstance::getSolutionValue(Id* id) {
  id = id->decl()->id();

  if(id->type().isvar()) {
    MIP_solver::Variable var = exprToVar(id);
    double val = getMIPWrapper()->getValues()[var];
    switch (id->type().bt()) {
      case Type::BT_INT: return IntLit::a(round_to_longlong(val));
      case Type::BT_FLOAT: return FloatLit::a(val);
      case Type::BT_BOOL: return new BoolLit(Location(), round_to_longlong(val));
      default: return NULL;
    }
  } else {
    return id->decl()->e();
  }
}


void MIP_solverinstance::assignSolutionToOutput() {
  if (mOutputDecls.empty()) {   // fill output decls map
     for (VarDeclIterator it = getEnv()->output()->begin_vardecls(); it != getEnv()->output()->end_vardecls(); ++it) {
        mOutputDecls[it->e()->id()->str().str()] = it->e();
     }
  }

  //iterate over set of ids that have an output annotation && obtain their right hand side from the flat model
  for(unsigned int i=0; i<_varsWithOutput.size(); i++) {
    VarDecl* vd = _varsWithOutput[i];
    //std::cout << "DEBUG: Looking at var-decl with output-annotation: " << *vd << std::endl;
    if(Call* output_array_ann = Expression::dyn_cast<Call>(getAnnotation(vd->ann(), constants().ann.output_array.aststr()))) {
      assert(vd->e());

      if(ArrayLit* al = vd->e()->dyn_cast<ArrayLit>()) {
        std::vector<Expression*> array_elems;
        ASTExprVec<Expression> array = al->v();
        for(unsigned int j=0; j<array.size(); j++) {
          if(Id* id = array[j]->dyn_cast<Id>()) {
            //std::cout << "DEBUG: getting solution value from " << *id  << " : " << id->v() << std::endl;
            array_elems.push_back(getSolutionValue(id));
          } else if(FloatLit* floatLit = array[j]->dyn_cast<FloatLit>()) {
            array_elems.push_back(floatLit);
          } else if(IntLit* intLit = array[j]->dyn_cast<IntLit>()) {
            array_elems.push_back(intLit);
          } else if(BoolLit* boolLit = array[j]->dyn_cast<BoolLit>()) {
            array_elems.push_back(boolLit);
          } else {
            std::cerr << "Error: array element " << *array[j] << " is ! an id nor a literal" << std::endl;
            assert(false);
          }
        }
        GCLock lock;
        ArrayLit* dims;
        Expression* e = output_array_ann->args()[0];
        if(ArrayLit* al = e->dyn_cast<ArrayLit>()) {
          dims = al;
        } else if(Id* id = e->dyn_cast<Id>()) {
          dims = id->decl()->e()->cast<ArrayLit>();
        } else {
          throw -1;
        }
        std::vector<std::pair<int,int> > dims_v;
        for( int i=0;i<dims->length();i++) {
          IntSetVal* isv = eval_intset(getEnv()->envi(), dims->v()[i]);
          dims_v.push_back(std::pair<int,int>(isv->min(0).toInt(),isv->max(isv->size()-1).toInt()));
        }
        ArrayLit* array_solution = new ArrayLit(Location(),array_elems,dims_v);
        KeepAlive ka(array_solution);
        // add solution to the output
//         for (VarDeclIterator it = getEnv()->output()->begin_vardecls(); it != getEnv()->output()->end_vardecls(); ++it) {
//           if(it->e()->id()->str() == vd->id()->str()) {
//             //std::cout << "DEBUG: Assigning array solution to " << it->e()->id()->str() << std::endl;
//             it->e()->e(array_solution); // set the solution
//           }
//         }
        auto it = mOutputDecls.find(vd->id()->str().str());
        if (mOutputDecls.end() != it)
          it->second->e(array_solution);
      }
    } else if(vd->ann().contains(constants().ann.output_var)) {
      Expression* sol = getSolutionValue(vd->id());
      vd->e(sol);
//       for (VarDeclIterator it = getEnv()->output()->begin_vardecls(); it != getEnv()->output()->end_vardecls(); ++it) {
//         if(it->e()->id()->str() == vd->id()->str()) {
//           //std::cout << "DEBUG: Assigning array solution to " << it->e()->id()->str() << std::endl;
//           it->e()->e(sol); // set the solution
//         }
//       }
      auto it = mOutputDecls.find(vd->id()->str().str());
      if (mOutputDecls.end() != it)
        it->second->e(sol);
    }
  }

}

