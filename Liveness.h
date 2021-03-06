//===- Hello.cpp - Example code from "Writing an LLVM Pass" ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements two versions of the LLVM "Hello World" pass described
// in docs/WritingAnLLVMPass.html
//
//===----------------------------------------------------------------------===//

#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IntrinsicInst.h"
#include <llvm/IR/InstIterator.h>
#include "Dataflow.h"

#include <vector>
#include <map>
#include <set>
using namespace llvm;

using FunctionSet = std::set<Function *>;
using ValueSet = std::set<Value *>;
using LiveVarsToMap = std::map<Value *, ValueSet>;

bool debug = false; //flag for debug

struct LivenessInfo
{
    //std::set<Instruction *> LiveVars; /// Set of variables which are live
    // p *p **p;
    LiveVarsToMap LiveVars_map;
    LiveVarsToMap LiveVars_feild_map;
    LivenessInfo() : LiveVars_map(), LiveVars_feild_map() {}
    LivenessInfo(const LivenessInfo &info) : LiveVars_map(info.LiveVars_map), LiveVars_feild_map(info.LiveVars_feild_map) {}

    bool operator==(const LivenessInfo &info) const
    {
        return (LiveVars_map == info.LiveVars_map) && (LiveVars_feild_map == info.LiveVars_feild_map);
    }

    bool operator!=(const LivenessInfo &info) const
    {
        return (LiveVars_map != info.LiveVars_map) || (LiveVars_feild_map != info.LiveVars_feild_map);
    }

    LivenessInfo &operator=(const LivenessInfo &info)
    {
        LiveVars_map = info.LiveVars_map;
        LiveVars_feild_map = info.LiveVars_feild_map;
        return *this;
    } // assign
};

inline raw_ostream &operator<<(raw_ostream &out, const LiveVarsToMap &v)
{
    out << "{ ";
    for (auto i = v.begin(), e = v.end(); i != e; ++i)
    {
        out << i->first->getName() << " " << i->first << " -> ";
        for (auto ii = i->second.begin(), ie = i->second.end(); ii != ie; ++ii)
        {
            if (ii != i->second.begin())
            {
                errs() << ", ";
            }
            out << (*ii)->getName() << " " << (*ii);
        }
        out << " ; ";
    }
    out << "}";
    return out;
}

class LivenessVisitor : public DataflowVisitor<struct LivenessInfo>
{
public:
    std::map<CallInst *, FunctionSet> call_func_result;
    FunctionSet fn_worklist;
    LivenessVisitor() : call_func_result(), fn_worklist() {}

    void merge(LivenessInfo *dest, const LivenessInfo &src) override
    {
        for (auto ii = src.LiveVars_map.begin(), ie = src.LiveVars_map.end(); ii != ie; ii++)
        {
            dest->LiveVars_map[ii->first].insert(ii->second.begin(), ii->second.end());
        }
        for (auto ii = src.LiveVars_feild_map.begin(), ie = src.LiveVars_feild_map.end(); ii != ie; ii++)
        {
            dest->LiveVars_feild_map[ii->first].insert(ii->second.begin(), ii->second.end());
        }
    }

    void HandlePHINode(PHINode *phiNode, DataflowResult<LivenessInfo>::Type *result)
    {
        LivenessInfo dfval = (*result)[phiNode].first;

        dfval.LiveVars_map[phiNode].clear();
        for (Value *value : phiNode->incoming_values())
        {
            if (isa<Function>(value))
            {
                dfval.LiveVars_map[phiNode].insert(value);
            }
            else
            {
                ValueSet &values = dfval.LiveVars_map[value];
                dfval.LiveVars_map[phiNode].insert(values.begin(), values.end());
            }
            // 对于PHI节点，Union进来的所有set
        }

        (*result)[phiNode].second = dfval;
    }

    void HandleCallInst(CallInst *callInst, DataflowResult<LivenessInfo>::Type *result)
    {
        LivenessInfo dfval = (*result)[callInst].first;

        FunctionSet callees;
        //callee被调用者，caller调用者
        Value *value = callInst->getCalledValue();
        if (auto *func = dyn_cast<Function>(value))
        {
            callees.insert(func);
        }
        else
        {
            ValueSet value_worklist;
            if (dfval.LiveVars_map.count(value))
            {
                value_worklist.insert(dfval.LiveVars_map[value].begin(), dfval.LiveVars_map[value].end());
            }

            while (!value_worklist.empty())
            {
                Value *v = *(value_worklist.begin());
                value_worklist.erase(value_worklist.begin());
                if (auto *func = dyn_cast<Function>(v))
                {
                    callees.insert(func);
                }
                else
                {
                    value_worklist.insert(dfval.LiveVars_map[v].begin(), dfval.LiveVars_map[v].end());
                }
                //前向访问找到所有的func
            }
        }

        call_func_result[callInst].clear();
        call_func_result[callInst].insert(callees.begin(), callees.end());

        /// Return the function called, or null if this is an
        /// indirect function invocation.
        if (callInst->getCalledFunction() && callInst->getCalledFunction()->isDeclaration())
        {
            (*result)[callInst].second = (*result)[callInst].first;
            return;
        }

        for (auto calleei = callees.begin(), calleee = callees.end(); calleei != calleee; calleei++)
        {
            Function *callee = *calleei;
            // 声明不算
            if (callee->isDeclaration())
            {
                continue;
            }
            std::map<Value *, Argument *> ValueToArg_map;

            for (int argi = 0, arge = callInst->getNumArgOperands(); argi < arge; argi++)
            {
                Value *caller_arg = callInst->getArgOperand(argi);
                if (caller_arg->getType()->isPointerTy())
                {
                    // only consider pointer
                    Argument *callee_arg = callee->arg_begin() + argi;
                    ValueToArg_map.insert(std::make_pair(caller_arg, callee_arg));
                }
            }

            if (ValueToArg_map.empty())
            {
                LivenessInfo tmpdfval = (*result)[callInst].second;
                merge(&tmpdfval, (*result)[callInst].first);
                continue;
            }

            // replace LiveVars_map
            LivenessInfo tmpdfval = (*result)[callInst].first;
            LivenessInfo &callee_dfval_in = (*result)[&*inst_begin(callee)].first;
            LivenessInfo old_callee_dfval_in = callee_dfval_in;
            for (auto bi = tmpdfval.LiveVars_map.begin(), be = tmpdfval.LiveVars_map.end(); bi != be; bi++)
            {
                for (auto argi = ValueToArg_map.begin(), arge = ValueToArg_map.end(); argi != arge; argi++)
                {
                    if (bi->second.count(argi->first) && !isa<Function>(argi->first))
                    {
                        // 函数
                        bi->second.erase(argi->first);
                        bi->second.insert(argi->second);
                    }
                }
            }

            // replace LiveVars_feild_map
            for (auto bi = tmpdfval.LiveVars_feild_map.begin(), be = tmpdfval.LiveVars_feild_map.end(); bi != be; bi++)
            {
                for (auto argi = ValueToArg_map.begin(), arge = ValueToArg_map.end(); argi != arge; argi++)
                {
                    if (bi->second.count(argi->first) && !isa<Function>(argi->first))
                    {
                        bi->second.erase(argi->first);
                        bi->second.insert(argi->second);
                    }
                }
            }

            for (auto argi = ValueToArg_map.begin(), arge = ValueToArg_map.end(); argi != arge; argi++)
            {
                if (tmpdfval.LiveVars_map.count(argi->first))
                {
                    ValueSet values = tmpdfval.LiveVars_map[argi->first];
                    tmpdfval.LiveVars_map.erase(argi->first);
                    tmpdfval.LiveVars_map[argi->second].insert(values.begin(), values.end());
                }

                if (tmpdfval.LiveVars_feild_map.count(argi->first))
                {
                    ValueSet values = tmpdfval.LiveVars_feild_map[argi->first];
                    tmpdfval.LiveVars_feild_map.erase(argi->first);
                    tmpdfval.LiveVars_feild_map[argi->second].insert(values.begin(), values.end());
                }
            }

            for (auto argi = ValueToArg_map.begin(), arge = ValueToArg_map.end(); argi != arge; argi++)
            {
                if (isa<Function>(argi->first))
                {
                    tmpdfval.LiveVars_map[argi->second].insert(argi->first);
                }
            }

            merge(&callee_dfval_in, tmpdfval);
            if (old_callee_dfval_in != callee_dfval_in)
            {
                fn_worklist.insert(callee);
            }
        }
    }

    void HandleStoreInst(StoreInst *storeInst, DataflowResult<LivenessInfo>::Type *result)
    {
        LivenessInfo dfval = (*result)[storeInst].first;

        ValueSet values;
        if (dfval.LiveVars_map[storeInst->getValueOperand()].empty())
        {
            values.insert(storeInst->getValueOperand());
        }
        else
        {
            ValueSet &tmp = dfval.LiveVars_map[storeInst->getValueOperand()];
            values.insert(tmp.begin(), tmp.end());
        }

        if (auto *getElementPtrInst = dyn_cast<GetElementPtrInst>(storeInst->getPointerOperand()))
        {
            Value *pointerOperand = getElementPtrInst->getPointerOperand();
            if (dfval.LiveVars_map[pointerOperand].empty())
            {
                dfval.LiveVars_feild_map[pointerOperand].clear();
                dfval.LiveVars_feild_map[pointerOperand].insert(values.begin(), values.end());
            }
            else
            {
                ValueSet &tmp = dfval.LiveVars_map[pointerOperand];
                for (auto tmpi = tmp.begin(), tmpe = tmp.end(); tmpi != tmpe; tmpi++)
                {
                    Value *v = *tmpi;
                    dfval.LiveVars_feild_map[v].clear();
                    dfval.LiveVars_feild_map[v].insert(values.begin(), values.end());
                }
            }
        }
        else
        {
            //ptr
            dfval.LiveVars_map[storeInst->getPointerOperand()].clear();
            dfval.LiveVars_map[storeInst->getPointerOperand()].insert(values.begin(), values.end());
        }

        (*result)[storeInst].second = dfval;
    }

    void HandleLoadInst(LoadInst *loadInst, DataflowResult<LivenessInfo>::Type *result)
    {
        LivenessInfo dfval = (*result)[loadInst].first;

        dfval.LiveVars_map[loadInst].clear();
        if (auto *getElementPtrInst = dyn_cast<GetElementPtrInst>(loadInst->getPointerOperand()))
        {
            Value *pointerOperand = getElementPtrInst->getPointerOperand();
            if (dfval.LiveVars_map[pointerOperand].empty())
            {
                ValueSet &tmp = dfval.LiveVars_feild_map[pointerOperand];
                dfval.LiveVars_map[loadInst].insert(tmp.begin(), tmp.end());
            }
            else
            {
                ValueSet &values = dfval.LiveVars_map[pointerOperand];
                for (auto valuei = values.begin(), valuee = values.end(); valuei != valuee; valuei++)
                {
                    Value *v = *valuei;
                    ValueSet &tmp = dfval.LiveVars_feild_map[v];
                    dfval.LiveVars_map[loadInst].insert(tmp.begin(), tmp.end());
                }
            }
        }
        else
        {
            // ptr
            ValueSet &tmp = dfval.LiveVars_map[loadInst->getPointerOperand()];
            dfval.LiveVars_map[loadInst].insert(tmp.begin(), tmp.end());
        }
        (*result)[loadInst].second = dfval;
    }

    void HandleReturnInst(ReturnInst *returnInst, DataflowResult<LivenessInfo>::Type *result)
    {
        LivenessInfo dfval = (*result)[returnInst].first;

        Function *callee = returnInst->getFunction();
        //前向找到哪个函数调用了return的函数
        for (auto funci = call_func_result.begin(), funce = call_func_result.end(); funci != funce; funci++)
        {
            if (funci->second.count(callee))
            { // funci call callee
                CallInst *callInst = funci->first;
                Function *caller = callInst->getFunction();

                std::map<Value *, Argument *> ValueToArg_map;
                for (unsigned argi = 0, arge = callInst->getNumArgOperands(); argi < arge; argi++)
                {
                    Value *caller_arg = callInst->getArgOperand(argi);
                    if (!caller_arg->getType()->isPointerTy())
                        continue;
                    Argument *callee_arg = callee->arg_begin() + argi;
                    ValueToArg_map.insert(std::make_pair(caller_arg, callee_arg));
                }

                LivenessInfo tmpdfval = (*result)[returnInst].first;
                LivenessInfo &caller_dfval_out = (*result)[callInst].second;
                LivenessInfo old_caller_dfval_out = caller_dfval_out;

                if (returnInst->getReturnValue() &&
                    returnInst->getReturnValue()->getType()->isPointerTy())
                {
                    ValueSet values = tmpdfval.LiveVars_map[returnInst->getReturnValue()];
                    tmpdfval.LiveVars_map.erase(returnInst->getReturnValue());
                    tmpdfval.LiveVars_map[callInst].insert(values.begin(), values.end());
                }
                // // replace LiveVars_map
                for (auto bi = tmpdfval.LiveVars_map.begin(), be = tmpdfval.LiveVars_map.end(); bi != be; bi++)
                {
                    for (auto argi = ValueToArg_map.begin(), arge = ValueToArg_map.end(); argi != arge; argi++)
                    {
                        if (bi->second.count(argi->second))
                        {
                            bi->second.erase(argi->second);
                            bi->second.insert(argi->first);
                        }
                    }
                }

                // replace LiveVars_feild_map
                for (auto bi = tmpdfval.LiveVars_feild_map.begin(), be = tmpdfval.LiveVars_feild_map.end(); bi != be; bi++)
                {
                    for (auto argi = ValueToArg_map.begin(), arge = ValueToArg_map.end(); argi != arge; argi++)
                    {
                        if (bi->second.count(argi->second))
                        {
                            bi->second.erase(argi->second);
                            bi->second.insert(argi->first);
                        }
                    }
                }
                for (auto argi = ValueToArg_map.begin(), arge = ValueToArg_map.end(); argi != arge; argi++)
                {
                    if (tmpdfval.LiveVars_map.count(argi->second))
                    {
                        ValueSet values = tmpdfval.LiveVars_map[argi->second];
                        tmpdfval.LiveVars_map.erase(argi->second);
                        tmpdfval.LiveVars_map[argi->first].insert(values.begin(), values.end());
                    }
                    if (tmpdfval.LiveVars_feild_map.count(argi->second))
                    {
                        ValueSet values = tmpdfval.LiveVars_feild_map[argi->second];
                        tmpdfval.LiveVars_feild_map.erase(argi->second);
                        tmpdfval.LiveVars_feild_map[argi->first].insert(values.begin(), values.end());
                    }
                }

                merge(&caller_dfval_out, tmpdfval);
                if (caller_dfval_out != old_caller_dfval_out)
                {
                    fn_worklist.insert(caller);
                }
            }
        }
        (*result)[returnInst].second = dfval;
    }

    void HandleGetElementPtrInst(GetElementPtrInst *getElementPtrInst, DataflowResult<LivenessInfo>::Type *result)
    {
        LivenessInfo dfval = (*result)[getElementPtrInst].first;

        dfval.LiveVars_map[getElementPtrInst].clear();

        Value *pointerOperand = getElementPtrInst->getPointerOperand();
        if (dfval.LiveVars_map[pointerOperand].empty())
        {
            dfval.LiveVars_map[getElementPtrInst].insert(pointerOperand);
        }
        else
        {
            dfval.LiveVars_map[getElementPtrInst].insert(dfval.LiveVars_map[pointerOperand].begin(), dfval.LiveVars_map[pointerOperand].end());
        }

        (*result)[getElementPtrInst].second = dfval;
    }

    void HandleBitCastInst(BitCastInst *bitCastInst, DataflowResult<LivenessInfo>::Type *result)
    {
        LivenessInfo dfval = (*result)[bitCastInst].first;
        (*result)[bitCastInst].second = dfval;
    }

    void HandleMemCpyInst(MemCpyInst *memCpyInst, DataflowResult<LivenessInfo>::Type *result)
    {
        LivenessInfo dfval = (*result)[memCpyInst].first;

        auto *b1 = dyn_cast<BitCastInst>(memCpyInst->getArgOperand(0));
        auto *b2 = dyn_cast<BitCastInst>(memCpyInst->getArgOperand(1));
        if (b1 && b2)
        {
            Value *dest = b1->getOperand(0);
            Value *src = b2->getOperand(0);
            dfval.LiveVars_map[dest].clear();
            dfval.LiveVars_map[dest].insert(dfval.LiveVars_map[src].begin(), dfval.LiveVars_map[src].end());

            dfval.LiveVars_feild_map[dest].clear();
            dfval.LiveVars_feild_map[dest].insert(dfval.LiveVars_feild_map[src].begin(), dfval.LiveVars_feild_map[src].end());
        }
        (*result)[memCpyInst].second = dfval;
    }

    void compDFVal(Instruction *inst, DataflowResult<LivenessInfo>::Type *result) override
    {
        if (isa<IntrinsicInst>(inst))
        {
            if (auto *memCpyInst = dyn_cast<MemCpyInst>(inst))
            {
                if (debug)
                {
                    errs() << "I am in MemCpyInst"
                           << "\n";
                }
                HandleMemCpyInst(memCpyInst, result);
            }
            else
            {
                (*result)[inst].second = (*result)[inst].first;
                return;
            }
        }
        else if (auto *phiNode = dyn_cast<PHINode>(inst))
        {
            if (debug)
            {
                errs() << "I am in PHINode"
                       << "\n";
            }
            HandlePHINode(phiNode, result);
        }
        else if (auto *callInst = dyn_cast<CallInst>(inst))
        {
            if (debug)
            {
                errs() << "I am in CallInst"
                       << "\n";
            }
            HandleCallInst(callInst, result);
        }
        else if (auto *storeInst = dyn_cast<StoreInst>(inst))
        {
            if (debug)
            {
                errs() << "I am in StoreInst"
                       << "\n";
            }
            HandleStoreInst(storeInst, result);
        }
        else if (auto *loadInst = dyn_cast<LoadInst>(inst))
        {
            if (debug)
            {
                errs() << "I am in LoadInst"
                       << "\n";
            }
            HandleLoadInst(loadInst, result);
        }
        else if (auto *returnInst = dyn_cast<ReturnInst>(inst))
        {
            if (debug)
            {
                errs() << "I am in ReturnInst"
                       << "\n";
            }
            HandleReturnInst(returnInst, result);
        }
        else if (auto *getElementPtrInst = dyn_cast<GetElementPtrInst>(inst))
        {
            if (debug)
            {
                errs() << "I am in GetElementPtrInst"
                       << "\n";
            }
            HandleGetElementPtrInst(getElementPtrInst, result);
        }
        else if (auto *bitCastInst = dyn_cast<BitCastInst>(inst))
        {
            if (debug)
            {
                errs() << "I am in bitCastInst"
                       << "\n";
            }
            HandleBitCastInst(bitCastInst, result);
        }
        else
        {
            if (debug)
            {
                errs() << "None of above"
                       << "\n";
            }
            (*result)[inst].second = (*result)[inst].first;
        }
        return;
    }

    void printCallFuncResult()
    {
        // 每次找到行数最小的，输出对应的结果，然后从call_func_result中删除
        while (!call_func_result.empty())
        {
            int line = call_func_result.begin()->first->getDebugLoc().getLine();
            auto p = call_func_result.begin();
            for (auto ii = call_func_result.begin(), ie = call_func_result.end(); ii != ie; ii++)
            {
                if (ii->first->getDebugLoc().getLine() < line)
                {
                    line = ii->first->getDebugLoc().getLine();
                    p = ii;
                }
            }
            errs() << line << " : ";
            for (auto fi = p->second.begin(), fe = p->second.end(); fi != fe; fi++)
            {
                if (fi != p->second.begin())
                {
                    errs() << ", ";
                }
                errs() << (*fi)->getName();
            }
            errs() << "\n";
            call_func_result.erase(p);
        }
    }
};

class Liveness : public FunctionPass
{
public:
    static char ID;
    Liveness() : FunctionPass(ID) {}

    bool runOnFunction(Function &F) override
    {
        //F.dump();
        F.print(llvm::errs(), nullptr);
        LivenessVisitor visitor;
        DataflowResult<LivenessInfo>::Type result;
        LivenessInfo initval;
        /*
        compBackwardDataflow(&F, &visitor, &result, initval);
        printDataflowResult<LivenessInfo>(errs(), result);
        */
        return false;
    }
};
