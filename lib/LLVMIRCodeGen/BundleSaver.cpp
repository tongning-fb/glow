/**
 * Copyright (c) Glow Contributors. See CONTRIBUTORS file.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "BundleSaver.h"
#include "CommandLine.h"

#include "glow/LLVMIRCodeGen/LLVMBackend.h"

#include "glow/Graph/Graph.h"
#include "glow/Graph/PlaceholderBindings.h"
#include "glow/IR/Instrs.h"
#include "glow/Support/Debug.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <glog/logging.h>

#define DEBUG_TYPE "jit"

using namespace glow;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

enum BundleApiType {
  /// Dynamic bundle API (default) with the following features:
  /// - the weights are exported in a binary file which are assumed
  ///   to be loaded dynamically at run-time.
  /// - the memory layout information (bundle configuration) is only
  ///   available at run-time and therefore allows ONLY dynamic memory
  ///   allocaton.
  Dynamic,
  /// Static bundle API with the following features:
  /// - the weights are exported in a binary file but and also in a
  ///   text file (C array format) suitable to include at compile-time.
  /// - the memory layout information (bundle configuration) is available
  ///   at compile-time through macros printed in the header file and thus
  ///   allows also static memory allocation.
  /// - this API is suitable for low end devices with no file system or OS
  ///   (bare-metal).
  Static,
};

namespace {
llvm::cl::OptionCategory bundleSaverCat("Bundle Options");

llvm::cl::opt<BundleApiType> bundleApi(
    "bundle-api", llvm::cl::desc("Specify which bundle API to use."),
    llvm::cl::Optional,
    llvm::cl::values(clEnumValN(BundleApiType::Dynamic, "dynamic",
                                "Dynamic API"),
                     clEnumValN(BundleApiType::Static, "static", "Static API")),
    llvm::cl::init(BundleApiType::Dynamic), llvm::cl::cat(bundleSaverCat));
} // namespace

/// Header file string template.
static const char *headerFileTemplate =
    R"RAW(// Bundle API header file
// Auto-generated file. Do not edit!
#ifndef _GLOW_BUNDLE_%s_H
#define _GLOW_BUNDLE_%s_H

#include <stdint.h>

// ---------------------------------------------------------------
//                       Common definitions                       
// ---------------------------------------------------------------
#ifndef _GLOW_BUNDLE_COMMON_DEFS
#define _GLOW_BUNDLE_COMMON_DEFS
%s
#endif

// ---------------------------------------------------------------
//                          Bundle API                            
// ---------------------------------------------------------------
%s
// NOTE: Placeholders are allocated within the "mutableWeight"
// buffer and are identified using an offset relative to base.
// ---------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
%s
#ifdef __cplusplus
}
#endif
#endif
)RAW";

/// Function to print the header file using the template.
static void printHeader(llvm::StringRef headerFileName,
                        llvm::StringRef bundleName,
                        llvm::StringRef commonDefines,
                        llvm::StringRef modelInfo, llvm::StringRef modelApi) {
  std::error_code EC;
  llvm::raw_fd_ostream headerFile(headerFileName, EC,
                                  llvm::sys::fs::OpenFlags::F_Text);
  CHECK(!EC) << "Could not open header file!";
  headerFile << strFormat(headerFileTemplate, bundleName.upper().data(),
                          bundleName.upper().data(), commonDefines.data(),
                          modelInfo.data(), modelApi.data());
  headerFile.close();
}

/// Header file common definitions for dynamic API.
static const char *dynamicApiCommonDefines = R"RAW(
// Type describing a symbol table entry of a generated bundle.
struct SymbolTableEntry {
  // Name of a variable.
  const char *name;
  // Offset of the variable inside the memory area.
  uint64_t offset;
  // The number of elements inside this variable.
  uint64_t size;
  // Variable kind: 1 if it is a mutable variable, 0 otherwise.
  char kind;
};

// Type describing the config of a generated bundle.
struct BundleConfig {
  // Size of the constant weight variables memory area.
  uint64_t constantWeightVarsMemSize;
  // Size of the mutable weight variables memory area.
  uint64_t mutableWeightVarsMemSize;
  // Size of the activations memory area.
  uint64_t activationsMemSize;
  // Alignment to be used for weights and activations.
  uint64_t alignment;
  // Number of symbols in the symbol table.
  uint64_t numSymbols;
  // Symbol table.
  const SymbolTableEntry *symbolTable;
};
)RAW";

/// Header file common definitions for static API.
static const char *staticApiCommonDefines = R"RAW(
// Memory alignment definition with given alignment size
// for static allocation of memory.
#define GLOW_MEM_ALIGN(size)  __attribute__((aligned(size)))

// Macro function to get the absolute address of a
// placeholder using the base address of the mutable
// weight buffer and placeholder offset definition.
#define GLOW_GET_ADDR(mutableBaseAddr, placeholderOff)  (((uint8_t*)(mutableBaseAddr)) + placeholderOff)
)RAW";

/// Utility function to serialize a binary file to text file as a C array.
static void serializeBinaryToText(llvm::StringRef binFileName,
                                  llvm::StringRef txtFileName) {
  FILE *inpFile = fopen(binFileName.str().c_str(), "rb");
  CHECK(inpFile) << "Could not open binary input file: " << binFileName.str();
  FILE *outFile = fopen(txtFileName.str().c_str(), "w");
  CHECK(outFile) << "Could not open text output file: " << txtFileName.str();
  const size_t numBytesPerLine = 20;
  for (size_t i = 0;; i++) {
    int ch = fgetc(inpFile);
    if (ch == EOF) {
      break;
    }
    fprintf(outFile, " 0X%02X,", ch);
    if ((i % numBytesPerLine) == (numBytesPerLine - 1)) {
      fprintf(outFile, "\n");
    }
  }
  fprintf(outFile, "\n");
  fclose(inpFile);
  fclose(outFile);
}

BundleSaver::BundleSaver(const IRFunction *F, const LLVMBackend &llvmBackend)
    : F_(F), irgen_(llvmBackend.createIRGen(F_, allocationsInfo_)) {}

void BundleSaver::saveWeights(llvm::StringRef weightsFileName) {
  std::error_code EC;
  llvm::raw_fd_ostream weightsFile(weightsFileName, EC, llvm::sys::fs::F_None);
  CHECK(!EC) << "Could not open the output file for saving the bundle weights "
                "with file name: "
             << weightsFileName.str();
  // Serialize only constant weights.
  // Do not serialize mutable weights representing inputs and outputs, because
  // it should be configurable and set by the client.
  size_t pos = 0;
  size_t maxPos = 0;
  for (auto &v : F_->findConstants()) {
    auto *w = cast<WeightVar>(F_->getWeightForNode(v));
    auto numBytes = w->getSizeInBytes();
    auto payload = v->getPayload().getUnsafePtr();
    auto addr = allocationsInfo_.allocatedAddress_[w];
    if (addr < pos) {
      // The payload was written already. It aliases something we have seen
      // already.
      continue;
    }
    weightsFile.seek(addr);
    CHECK(!weightsFile.has_error()) << "Could not set file write position";
    weightsFile.write(payload, numBytes);
    CHECK(!weightsFile.has_error()) << "Could not write bytes";
    pos = addr + numBytes;
    maxPos = std::max(pos, maxPos);
  }
  // Make sure that the file is as long as the constantWeightVarsMemSize_.
  // This is needed to properly handle alignments.
  weightsFile.seek(maxPos);
  for (size_t endPos = irgen_->getAllocationsInfo().constantWeightVarsMemSize_;
       maxPos < endPos; maxPos++) {
    weightsFile.write(0);
  }
  weightsFile.close();
}

void BundleSaver::saveHeader(llvm::StringRef headerFileName) {
  auto bundleName = irgen_->getBundleName();
  auto bundleNameUpper = llvm::StringRef(bundleName).upper();
  auto constMemSize = irgen_->getAllocationsInfo().constantWeightVarsMemSize_;
  auto mutableMemSize = irgen_->getAllocationsInfo().mutableWeightVarsMemSize_;
  auto activationsMemSize = irgen_->getAllocationsInfo().activationsMemSize_;
  auto memAlignSize = TensorAlignment;
  auto totMemSize = constMemSize + mutableMemSize + activationsMemSize;

  // Format common bundle definitions.
  auto commonDefines = (bundleApi == BundleApiType::Dynamic)
                           ? dynamicApiCommonDefines
                           : staticApiCommonDefines;

  // Format model description.
  std::string modelInfo = strFormat("// Model name: \"%s\"\n"
                                    "// Total data size: %lu (bytes)\n"
                                    "// Placeholders:\n",
                                    bundleName.data(), totMemSize);
  for (auto &v : F_->findPlaceholders()) {
    auto *w = cast<WeightVar>(F_->getWeightForNode(v));
    // Get placeholder shape as string.
    std::string shapeStr = "[";
    auto dims = w->getType()->dims();
    for (size_t idx = 0; idx < dims.size(); idx++) {
      if (idx < dims.size() - 1) {
        shapeStr += strFormat("%lu, ", dims[idx]);
      } else {
        shapeStr += strFormat("%lu]", dims[idx]);
      }
    }
    // Get placeholder properties.
    auto name = w->getName();
    auto typeName = w->getType()->getElementName();
    auto sizeElem = w->getType()->size();
    auto sizeByte = w->getType()->getSizeInBytes();
    auto offset = allocationsInfo_.allocatedAddress_[w];
    modelInfo += strFormat("//\n"
                           "//   Name: \"%s\"\n"
                           "//   Type: %s\n"
                           "//   Shape: %s\n"
                           "//   Size: %zu (elements)\n"
                           "//   Size: %zu (bytes)\n"
                           "//   Offset: %lu (bytes)\n",
                           name.data(), typeName.data(), shapeStr.c_str(),
                           sizeElem, sizeByte, offset);
  }
  modelInfo += "//";

  std::string modelApi = "\n";
  if (bundleApi == BundleApiType::Dynamic) {
    // Print bundle memory configuration.
    modelApi += strFormat("// Bundle memory configuration (memory layout)\n"
                          "extern BundleConfig %s_config;\n"
                          "\n",
                          bundleName.data());

  } else {
    // Get placeholder names and offsets. Compute also the maximum placeholder
    // name length for print purposes.
    unsigned nameMaxLen = 0;
    std::vector<std::pair<llvm::StringRef, unsigned>> nameAddrPairs;
    for (auto &v : F_->findPlaceholders()) {
      auto *w = cast<WeightVar>(F_->getWeightForNode(v));
      auto name = w->getName();
      auto addr = allocationsInfo_.allocatedAddress_[w];
      nameMaxLen = name.size() > nameMaxLen ? name.size() : nameMaxLen;
      nameAddrPairs.push_back(std::pair<llvm::StringRef, unsigned>(name, addr));
    }

    // Print placeholder address offsets.
    modelApi +=
        "// Placeholder address offsets within mutable buffer (bytes)\n";
    for (auto &pair : nameAddrPairs) {
      modelApi += strFormat(
          "#define %s_%s%s  %u\n", bundleNameUpper.data(), pair.first.data(),
          std::string(nameMaxLen - pair.first.size(), ' ').c_str(),
          pair.second);
    }
    modelApi += "\n";

    // Print memory sizes and memory alignment.
    modelApi +=
        strFormat("// Memory sizes (bytes)\n"
                  "#define %s_CONSTANT_MEM_SIZE     %lu\n"
                  "#define %s_MUTABLE_MEM_SIZE      %lu\n"
                  "#define %s_ACTIVATIONS_MEM_SIZE  %lu\n"
                  "\n"
                  "// Memory alignment (bytes)\n"
                  "#define %s_MEM_ALIGN  %d\n"
                  "\n",
                  bundleNameUpper.data(), constMemSize, bundleNameUpper.data(),
                  mutableMemSize, bundleNameUpper.data(), activationsMemSize,
                  bundleNameUpper.data(), memAlignSize);
  }

  // Print bundle entry function.
  modelApi += strFormat("// Bundle entry point (inference function)\n"
                        "void %s("
                        "uint8_t *constantWeight, "
                        "uint8_t *mutableWeight, "
                        "uint8_t *activations"
                        ");\n",
                        bundleName.data());

  // Print header file.
  printHeader(headerFileName, bundleName, commonDefines, modelInfo, modelApi);
}

void BundleSaver::emitSymbolTable() {
  // Define a struct for symbol table entries:
  // struct SymbolTableEntry {
  //  const char *name;
  //  uint64_t offset;
  //  uint64_t size;
  //  char kind;
  // };
  auto *charTy = llvm::Type::getInt8Ty(irgen_->getLLVMContext());
  auto *uint64TTy =
      llvm::Type::getIntNTy(irgen_->getLLVMContext(), sizeof(uint64_t) * 8);
  auto symbolTableEntryTy = llvm::StructType::get(
      irgen_->getLLVMContext(),
      {charTy->getPointerTo(), uint64TTy, uint64TTy, charTy});
  // Set of entries in the symbol table.
  llvm::SmallVector<llvm::Constant *, 128> entries;
  // Iterate over all Placeholders and record information about their names,
  // offset, size and kind.
  for (auto &v : F_->findPlaceholders()) {
    auto *w = cast<WeightVar>(F_->getWeightForNode(v));
    auto size = w->getType()->size();
    auto addr = allocationsInfo_.allocatedAddress_[w];
    // Create an SymbolTableEntry.
    auto *entry = llvm::ConstantStruct::get(
        symbolTableEntryTy,
        {// name.
         dyn_cast<llvm::Constant>(irgen_->getBuilder().CreateBitCast(
             irgen_->emitStringConst(irgen_->getBuilder(), w->getName()),
             charTy->getPointerTo())),
         // offset.
         llvm::ConstantInt::get(uint64TTy, addr),
         // size.
         llvm::ConstantInt::get(uint64TTy, size),
         // 1 for Mutable Kind
         llvm::ConstantInt::get(charTy, 1)});
    entries.push_back(entry);
  }

  // Create a constant array with these entries.
  auto *arr = llvm::ConstantArray::get(
      llvm::ArrayType::get(symbolTableEntryTy, entries.size()), entries);
  // Create a global variable and initialize it with the constructed array.
  new llvm::GlobalVariable(irgen_->getModule(), arr->getType(), true,
                           llvm::GlobalValue::InternalLinkage, arr,
                           irgen_->getMainEntryName() + "SymbolTable");
}

void BundleSaver::produceBundle(llvm::StringRef outputDir) {
  // Emit symbol table and bundle config only for dynamic API
  if (bundleApi == BundleApiType::Dynamic) {
    // Emit the symbol table for weight variables.
    emitSymbolTable();
    // Emit the config for the bundle.
    emitBundleConfig();
  }

  auto &M = irgen_->getModule();
  auto bundleName = irgen_->getBundleName();
  std::string extension = (llvmCompiler.empty()) ? ".o" : ".bc";
  auto bundleCodeOutput = (outputDir + "/" + bundleName + extension).str();
  auto bundleWeightsOutput = (outputDir + "/" + bundleName + ".weights").str();
  auto bundleHeaderOutput = (outputDir + "/" + bundleName + ".h").str();
  DEBUG_GLOW(llvm::dbgs() << "Producing a bundle:\n"
                          << "bundle name: " << bundleName << "\n"
                          << "bundle code: " << bundleCodeOutput << "\n"
                          << "bundle weights:" << bundleWeightsOutput << "\n"
                          << "header file: " << bundleHeaderOutput << "\n");
  llvm::StringRef fileName = bundleCodeOutput;
  std::error_code EC;
  llvm::raw_fd_ostream outputFile(fileName, EC, llvm::sys::fs::F_None);
  CHECK(!EC) << "Could not open the output file for saving the bundle "
                "code with file name: "
             << fileName.str();
  if (fileName.endswith(".bc")) {
    // Emit the bitcode file.
    llvm::WriteBitcodeToFile(M, outputFile);
    outputFile.flush();
    if (!llvmCompiler.empty()) {
      // Compile bitcode using an external LLVM compiler.
      std::string cmd = llvmCompiler;
      for (auto option : llvmCompilerOptions) {
        cmd += " " + option + " ";
      }
      cmd += " " + bundleCodeOutput;
      std::string bundleObjectCodeOutputOpt =
          " -o " + (outputDir + "/" + bundleName + ".o").str();
      cmd += bundleObjectCodeOutputOpt;
      CHECK(!system(cmd.c_str()))
          << "Error running external LLVM compiler: " << cmd;
    }
  } else if (fileName.endswith(".o")) {
    // Emit the object file.
    llvm::legacy::PassManager PM;
    auto &TM = irgen_->getTargetMachine();

#if FACEBOOK_INTERNAL && LLVM_VERSION_MAJOR < 8
    TM.addPassesToEmitFile(
        PM, outputFile, llvm::TargetMachine::CodeGenFileType::CGFT_ObjectFile);
#else
    TM.addPassesToEmitFile(
        PM, outputFile, nullptr,
        llvm::TargetMachine::CodeGenFileType::CGFT_ObjectFile);
#endif

    PM.run(M);
  }
  outputFile.close();
  // Output weights.
  saveWeights(bundleWeightsOutput);
  // Header file.
  saveHeader(bundleHeaderOutput);
  // Save weights also in text format for Static API.
  if (bundleApi == BundleApiType::Static) {
    auto bundleWeightsTxtOut = (outputDir + "/" + bundleName + ".inc").str();
    serializeBinaryToText(bundleWeightsOutput, bundleWeightsTxtOut);
  }
}

/// Emit the entry function for the bundle. It simply calls the main entry of
/// the module and forwards its arguments to it. As the last argument it
/// provides the constant array of offsets. Since these offsets are constants,
/// the LLVM optimizer will constant propagate them into relative addressing
/// computations and the like and produce a very efficient code that uses
/// absolute addressing whenever possible.
void BundleSaver::emitBundleEntryFunction() {
  // The bundle entry point has the following API:
  // void entry(uint8_t *baseConstantWeightVars, uint8_t *baseInoutWeightVars,
  // uint8_t *baseActivations);
  llvm::Type *voidTy = llvm::Type::getVoidTy(irgen_->getLLVMContext());
  auto int8PtrTy = llvm::Type::getInt8PtrTy(irgen_->getLLVMContext());
  llvm::FunctionType *bundleFuncTy =
      llvm::FunctionType::get(voidTy, {int8PtrTy, int8PtrTy, int8PtrTy}, false);
  auto *func =
      llvm::Function::Create(bundleFuncTy, llvm::Function::ExternalLinkage,
                             irgen_->getMainEntryName(), &irgen_->getModule());
  llvm::BasicBlock *entry_bb =
      llvm::BasicBlock::Create(irgen_->getLLVMContext(), "entry", func);
  llvm::IRBuilder<> builder(entry_bb);

  // Prepare arguments for the "main" function.
  llvm::SmallVector<llvm::Value *, 4> initFunctionCallArgs;
  initFunctionCallArgs.push_back(func->args().begin());
  initFunctionCallArgs.push_back(func->args().begin() + 1);
  initFunctionCallArgs.push_back(func->args().begin() + 2);
  // Now form the offsets array and pass it as the last argument.
  auto offsetsArray = irgen_->emitConstOffsetsArray(builder, allocationsInfo_);
  initFunctionCallArgs.push_back(offsetsArray);
  // Invoke the main entry with constant arguments and let LLVM optimizer make
  // use of it.
  auto *entryF = irgen_->getModule().getFunction("main");
  entryF->setLinkage(llvm::Function::InternalLinkage);
  irgen_->createCall(builder, entryF, initFunctionCallArgs);
  // Terminate the function.
  builder.CreateRetVoid();
  // Create the debug info for the bundle entry point function.
  irgen_->generateFunctionDebugInfo(func);
}

// Create a config for this network. It will be exposed to the clients,
// so that they know how much memory they need to allocate, etc.
// Config consists of the following fields:
// struct BundleConfig {
//   uint64_t constantWeightVarsMemSize;
//   uint64_t mutableWeightVarsMemSize;
//   uint64_t activationsMemSize;
//   uint64_t alignment;
//   uint64_t numSymbols;
//   SymbolTableEntry *symbolTable;
// };
void BundleSaver::emitBundleConfig() {
  auto symbolTableName = irgen_->getMainEntryName() + "SymbolTable";
  auto symbolTable =
      irgen_->getModule().getGlobalVariable(symbolTableName, true);
  CHECK(symbolTable)
      << "Expected to find a symbol table for the AOT bundle with name: "
      << symbolTableName;
  // Get the integer type having the same size in bits as uint64_t.
  auto *uint64TType = irgen_->getBuilder().getIntNTy(sizeof(uint64_t) * 8);
  auto symbolTableEntryTy = symbolTable->getType()->getPointerElementType();
  auto *bundleConfigTy =
      llvm::StructType::get(irgen_->getLLVMContext(),
                            {uint64TType, uint64TType, uint64TType, uint64TType,
                             uint64TType, symbolTableEntryTy->getPointerTo()});
  auto config = new llvm::GlobalVariable(
      irgen_->getModule(), bundleConfigTy, /* isConst */ true,
      llvm::GlobalValue::LinkageTypes::ExternalLinkage, nullptr,
      irgen_->getMainEntryName() + "_config");
  config->setInitializer(llvm::ConstantStruct::get(
      bundleConfigTy,
      llvm::ConstantInt::get(
          uint64TType, irgen_->getAllocationsInfo().constantWeightVarsMemSize_),
      llvm::ConstantInt::get(
          uint64TType, irgen_->getAllocationsInfo().mutableWeightVarsMemSize_),
      llvm::ConstantInt::get(uint64TType,
                             irgen_->getAllocationsInfo().activationsMemSize_),

      llvm::ConstantInt::get(uint64TType, TensorAlignment),
      llvm::ConstantInt::get(uint64TType, F_->findPlaceholders().size()),

      symbolTable));
}

void BundleSaver::performBundleMemoryAllocation() {
  allocationsInfo_.numberValues(F_);
  allocationsInfo_.allocateActivations(F_);
  // Tell the allocateWeightVars to not reuse any existing addresses for weights
  // and to assign new ones.
  allocationsInfo_.allocateWeightVars(F_);
  allocationsInfo_.allocateTensorViews(F_);
}

void BundleSaver::save(llvm::StringRef target, llvm::StringRef arch,
                       llvm::StringRef cpu,
                       const llvm::SmallVectorImpl<std::string> &targetFeatures,
                       llvm::StringRef outputDir, llvm::StringRef bundleName,
                       llvm::StringRef mainEntryName,
                       llvm::CodeModel::Model codeModel,
                       llvm::Reloc::Model relocModel) {
  // Object files generation works properly only in small mode.
  irgen_->initTargetMachine(target, arch, cpu, targetFeatures, codeModel,
                            relocModel);
  irgen_->setOutputDir(outputDir);
  irgen_->setBundleName(bundleName);
  irgen_->setMainEntryName(mainEntryName);
  irgen_->initCodeGen();
  // Perform the address assignment for activations and WeightVars.
  performBundleMemoryAllocation();
  // Create the bundle entry function.
  emitBundleEntryFunction();
  // Emit the code for the body of the entry function.
  irgen_->performCodeGen();
  // Produce the bundle.
  produceBundle(outputDir);
}
