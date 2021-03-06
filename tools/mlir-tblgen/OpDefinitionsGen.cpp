//===- OpDefinitionsGen.cpp - MLIR op definitions generator ---------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// OpDefinitionsGen uses the description of operations to generate C++
// definitions for ops.
//
//===----------------------------------------------------------------------===//

#include "mlir/Support/STLExtras.h"
#include "mlir/TableGen/Format.h"
#include "mlir/TableGen/GenInfo.h"
#include "mlir/TableGen/OpTrait.h"
#include "mlir/TableGen/Operator.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Signals.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"

using namespace llvm;
using namespace mlir;
using namespace mlir::tblgen;

static const char *const tblgenNamePrefix = "tblgen_";
static const char *const generatedArgName = "tblgen_arg";
static const char *const builderOpState = "tblgen_state";

static const char *const opCommentHeader = R"(
//===----------------------------------------------------------------------===//
// {0} {1}
//===----------------------------------------------------------------------===//

)";

//===----------------------------------------------------------------------===//
// Utility structs and functions
//===----------------------------------------------------------------------===//

// Returns whether the record has a value of the given name that can be returned
// via getValueAsString.
static inline bool hasStringAttribute(const Record &record,
                                      StringRef fieldName) {
  auto valueInit = record.getValueInit(fieldName);
  return isa<CodeInit>(valueInit) || isa<StringInit>(valueInit);
}

static std::string getArgumentName(const Operator &op, int index) {
  const auto &operand = op.getOperand(index);
  if (!operand.name.empty())
    return operand.name;
  else
    return formatv("{0}_{1}", generatedArgName, index);
}

namespace {
// Simple RAII helper for defining ifdef-undef-endif scopes.
class IfDefScope {
public:
  IfDefScope(StringRef name, raw_ostream &os) : name(name), os(os) {
    os << "#ifdef " << name << "\n"
       << "#undef " << name << "\n\n";
  }

  ~IfDefScope() { os << "\n#endif  // " << name << "\n\n"; }

private:
  StringRef name;
  raw_ostream &os;
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Classes for C++ code emission
//===----------------------------------------------------------------------===//

// We emit the op declaration and definition into separate files: *Ops.h.inc
// and *Ops.cpp.inc. The former is to be included in the dialect *Ops.h and
// the latter for dialect *Ops.cpp. This way provides a cleaner interface.
//
// In order to do this split, we need to track method signature and
// implementation logic separately. Signature information is used for both
// declaration and definition, while implementation logic is only for
// definition. So we have the following classes for C++ code emission.

namespace {
// Class for holding the signature of an op's method for C++ code emission
class OpMethodSignature {
public:
  OpMethodSignature(StringRef retType, StringRef name, StringRef params);

  // Writes the signature as a method declaration to the given `os`.
  void writeDeclTo(raw_ostream &os) const;
  // Writes the signature as the start of a method definition to the given `os`.
  // `namePrefix` is the prefix to be prepended to the method name (typically
  // namespaces for qualifying the method definition).
  void writeDefTo(raw_ostream &os, StringRef namePrefix) const;

private:
  // Returns true if the given C++ `type` ends with '&' or '*'.
  static bool endsWithRefOrPtr(StringRef type);

  std::string returnType;
  std::string methodName;
  std::string parameters;
};

// Class for holding the body of an op's method for C++ code emission
class OpMethodBody {
public:
  explicit OpMethodBody(bool declOnly);

  OpMethodBody &operator<<(Twine content);
  OpMethodBody &operator<<(int content);
  OpMethodBody &operator<<(const FmtObjectBase &content);

  void writeTo(raw_ostream &os) const;

private:
  // Whether this class should record method body.
  bool isEffective;
  std::string body;
};

// Class for holding an op's method for C++ code emission
class OpMethod {
public:
  // Properties (qualifiers) of class methods. Bitfield is used here to help
  // querying properties.
  enum Property {
    MP_None = 0x0,
    MP_Static = 0x1, // Static method
  };

  OpMethod(StringRef retType, StringRef name, StringRef params,
           Property property, bool declOnly);

  OpMethodSignature &signature();
  OpMethodBody &body();

  // Returns true if this is a static method.
  bool isStatic() const;

  // Writes the method as a declaration to the given `os`.
  void writeDeclTo(raw_ostream &os) const;
  // Writes the method as a definition to the given `os`. `namePrefix` is the
  // prefix to be prepended to the method name (typically namespaces for
  // qualifying the method definition).
  void writeDefTo(raw_ostream &os, StringRef namePrefix) const;

private:
  Property properties;
  // Whether this method only contains a declaration.
  bool isDeclOnly;
  OpMethodSignature methodSignature;
  OpMethodBody methodBody;
};

// Class for holding an op for C++ code emission
class OpClass {
public:
  explicit OpClass(StringRef name);

  // Adds an op trait.
  void addTrait(Twine trait);

  // Creates a new method in this op's class.
  OpMethod &newMethod(StringRef retType, StringRef name, StringRef params = "",
                      OpMethod::Property = OpMethod::MP_None,
                      bool declOnly = false);

  // Writes this op's class as a declaration to the given `os`.
  void writeDeclTo(raw_ostream &os) const;
  // Writes the method definitions in this op's class to the given `os`.
  void writeDefTo(raw_ostream &os) const;

private:
  std::string className;
  SmallVector<std::string, 4> traits;
  SmallVector<OpMethod, 8> methods;
};
} // end anonymous namespace

OpMethodSignature::OpMethodSignature(StringRef retType, StringRef name,
                                     StringRef params)
    : returnType(retType), methodName(name), parameters(params) {}

void OpMethodSignature::writeDeclTo(raw_ostream &os) const {
  os << returnType << (endsWithRefOrPtr(returnType) ? "" : " ") << methodName
     << "(" << parameters << ")";
}

void OpMethodSignature::writeDefTo(raw_ostream &os,
                                   StringRef namePrefix) const {
  // We need to remove the default values for parameters in method definition.
  // TODO(antiagainst): We are using '=' and ',' as delimiters for parameter
  // initializers. This is incorrect for initializer list with more than one
  // element. Change to a more robust approach.
  auto removeParamDefaultValue = [](StringRef params) {
    std::string result;
    std::pair<StringRef, StringRef> parts;
    while (!params.empty()) {
      parts = params.split("=");
      result.append(result.empty() ? "" : ", ");
      result.append(parts.first);
      params = parts.second.split(",").second;
    }
    return result;
  };

  os << returnType << (endsWithRefOrPtr(returnType) ? "" : " ") << namePrefix
     << (namePrefix.empty() ? "" : "::") << methodName << "("
     << removeParamDefaultValue(parameters) << ")";
}

bool OpMethodSignature::endsWithRefOrPtr(StringRef type) {
  return type.endswith("&") || type.endswith("*");
};

OpMethodBody::OpMethodBody(bool declOnly) : isEffective(!declOnly) {}

OpMethodBody &OpMethodBody::operator<<(Twine content) {
  if (isEffective)
    body.append(content.str());
  return *this;
}

OpMethodBody &OpMethodBody::operator<<(int content) {
  if (isEffective)
    body.append(std::to_string(content));
  return *this;
}

OpMethodBody &OpMethodBody::operator<<(const FmtObjectBase &content) {
  if (isEffective)
    body.append(content.str());
  return *this;
}

void OpMethodBody::writeTo(raw_ostream &os) const {
  os << body;
  if (body.empty() || body.back() != '\n')
    os << "\n";
}

OpMethod::OpMethod(StringRef retType, StringRef name, StringRef params,
                   OpMethod::Property property, bool declOnly)
    : properties(property), isDeclOnly(declOnly),
      methodSignature(retType, name, params), methodBody(declOnly) {}

OpMethodSignature &OpMethod::signature() { return methodSignature; }

OpMethodBody &OpMethod::body() { return methodBody; }

bool OpMethod::isStatic() const { return properties & MP_Static; }

void OpMethod::writeDeclTo(raw_ostream &os) const {
  os.indent(2);
  if (isStatic())
    os << "static ";
  methodSignature.writeDeclTo(os);
  os << ";";
}

void OpMethod::writeDefTo(raw_ostream &os, StringRef namePrefix) const {
  if (isDeclOnly)
    return;

  methodSignature.writeDefTo(os, namePrefix);
  os << " {\n";
  methodBody.writeTo(os);
  os << "}";
}

OpClass::OpClass(StringRef name) : className(name) {}

// Adds the given trait to this op. Prefixes "OpTrait::" to `trait` implicitly.
void OpClass::addTrait(Twine trait) {
  traits.push_back(("OpTrait::" + trait).str());
}

OpMethod &OpClass::newMethod(StringRef retType, StringRef name,
                             StringRef params, OpMethod::Property property,
                             bool declOnly) {
  methods.emplace_back(retType, name, params, property, declOnly);
  return methods.back();
}

void OpClass::writeDeclTo(raw_ostream &os) const {
  os << "class " << className << " : public Op<" << className;
  for (const auto &trait : traits)
    os << ", " << trait;
  os << "> {\npublic:\n";
  os << "  using Op::Op;\n";
  for (const auto &method : methods) {
    method.writeDeclTo(os);
    os << "\n";
  }
  os << "};";
}

void OpClass::writeDefTo(raw_ostream &os) const {
  for (const auto &method : methods) {
    method.writeDefTo(os, className);
    os << "\n\n";
  }
}

//===----------------------------------------------------------------------===//
// Op emitter
//===----------------------------------------------------------------------===//

namespace {
// Helper class to emit a record into the given output stream.
class OpEmitter {
public:
  static void emitDecl(const Record &def, raw_ostream &os);
  static void emitDef(const Record &def, raw_ostream &os);

private:
  OpEmitter(const Record &def);

  void emitDecl(raw_ostream &os);
  void emitDef(raw_ostream &os);

  // Generates getters for the attributes.
  void genAttrGetters();

  // Generates getters for named operands.
  void genNamedOperandGetters();

  // Generates getters for named results.
  void genNamedResultGetters();

  // Generates builder method for the operation.
  void genBuilder();

  // Generates canonicalizer declaration for the operation.
  void genCanonicalizerDecls();

  // Generates the folder declaration for the operation.
  void genFolderDecls();

  // Generates the parser for the operation.
  void genParser();

  // Generates the printer for the operation.
  void genPrinter();

  // Generates verify method for the operation.
  void genVerifier();

  // Generates the traits used by the object.
  void genTraits();

  // Generates the build() method that takes each result-type/operand/attribute
  // as a stand-alone parameter. Using the first operand's type as all result
  // types if `useOperandType` is true. Using the first attribute's type as all
  // result types if `useAttrType` true. Don't set `useOperandType` and
  // `useAttrType` at the same time.
  void genStandaloneParamBuilder(bool useOperandType, bool useAttrType);

  void genOpNameGetter();

  // The TableGen record for this op.
  const Record &def;

  // The wrapper operator class for querying information from this op.
  Operator op;

  // The C++ code builder for this op
  OpClass opClass;
};
} // end anonymous namespace

OpEmitter::OpEmitter(const Record &def)
    : def(def), op(def), opClass(op.getCppClassName()) {
  genTraits();
  // Generate C++ code for various op methods. The order here determines the
  // methods in the generated file.
  genOpNameGetter();
  genNamedOperandGetters();
  genNamedResultGetters();
  genAttrGetters();
  genBuilder();
  genParser();
  genPrinter();
  genVerifier();
  genCanonicalizerDecls();
  genFolderDecls();
}

void OpEmitter::emitDecl(const Record &def, raw_ostream &os) {
  OpEmitter(def).emitDecl(os);
}

void OpEmitter::emitDef(const Record &def, raw_ostream &os) {
  OpEmitter(def).emitDef(os);
}

void OpEmitter::emitDecl(raw_ostream &os) { opClass.writeDeclTo(os); }

void OpEmitter::emitDef(raw_ostream &os) { opClass.writeDefTo(os); }

void OpEmitter::genAttrGetters() {
  FmtContext fctx;
  fctx.withBuilder("mlir::Builder(this->getContext())");
  for (auto &namedAttr : op.getAttributes()) {
    const auto &name = namedAttr.name;
    const auto &attr = namedAttr.attr;

    auto &method = opClass.newMethod(attr.getReturnType(), name);
    auto &body = method.body();

    // Emit the derived attribute body.
    if (attr.isDerivedAttr()) {
      body << "  " << attr.getDerivedCodeBody() << "\n";
      continue;
    }

    // Emit normal emitter.

    // Return the queried attribute with the correct return type.
    auto attrVal = formatv("this->getAttr(\"{0}\").dyn_cast_or_null<{1}>()",
                           name, attr.getStorageType());
    body << "  auto attr = " << attrVal << ";\n";
    if (attr.hasDefaultValueInitializer()) {
      // Returns the default value if not set.
      // TODO: this is inefficient, we are recreating the attribute for every
      // call. This should be set instead.
      std::string defaultValue = tgfmt(attr.getConstBuilderTemplate(), &fctx,
                                       attr.getDefaultValueInitializer());
      body << "    if (!attr)\n      return "
           << tgfmt(attr.getConvertFromStorageCall(),
                    &fctx.withSelf(defaultValue))
           << ";\n";
    }
    body << "  return "
         << tgfmt(attr.getConvertFromStorageCall(), &fctx.withSelf("attr"))
         << ";\n";
  }
}

void OpEmitter::genNamedOperandGetters() {
  for (int i = 0, e = op.getNumOperands(); i != e; ++i) {
    const auto &operand = op.getOperand(i);
    if (operand.name.empty())
      continue;

    if (!operand.constraint.isVariadic()) {
      auto &m = opClass.newMethod("Value *", operand.name);
      m.body() << "  return this->getOperation()->getOperand(" << i << ");\n";
    } else {
      assert(i + 1 == e && "only the last operand can be variadic");

      const char *const code = R"(
        assert(getOperation()->getNumOperands() >= {0});
        return {std::next(operand_begin(), {0}), operand_end()};
      )";
      auto &m = opClass.newMethod("Operation::operand_range", operand.name);
      m.body() << formatv(code, i);
    }
  }
}

void OpEmitter::genNamedResultGetters() {
  for (int i = 0, e = op.getNumResults(); i != e; ++i) {
    const auto &result = op.getResult(i);
    if (result.constraint.isVariadic() || result.name.empty())
      continue;

    auto &m = opClass.newMethod("Value *", result.name);
    m.body() << "  return this->getOperation()->getResult(" << i << ");\n";
  }
}

void OpEmitter::genStandaloneParamBuilder(bool useOperandType,
                                          bool useAttrType) {
  if (useOperandType && useAttrType) {
    PrintFatalError(def.getLoc(),
                    "Op definition has both 'SameOperandsAndResultType' and "
                    "'FirstAttrIsResultType' trait specified.");
  }

  auto numResults = op.getNumResults();
  llvm::SmallVector<std::string, 4> resultNames;
  resultNames.reserve(numResults);

  std::string paramList = "Builder *, OperationState *";
  paramList.append(builderOpState);

  // Emit parameters for all return types
  if (!useOperandType && !useAttrType) {
    for (unsigned i = 0; i != numResults; ++i) {
      std::string resultName = op.getResultName(i);
      if (resultName.empty())
        resultName = formatv("resultType{0}", i);

      bool isVariadic = op.getResultTypeConstraint(i).isVariadic();
      paramList.append(isVariadic ? ", ArrayRef<Type> " : ", Type ");
      paramList.append(resultName);

      resultNames.emplace_back(std::move(resultName));
    }
  }

  // Emit parameters for all arguments (operands and attributes).
  int numOperands = 0;
  int numAttrs = 0;
  for (int i = 0, e = op.getNumArgs(); i < e; ++i) {
    auto argument = op.getArg(i);
    if (argument.is<tblgen::NamedTypeConstraint *>()) {
      auto &operand = op.getOperand(numOperands);
      paramList.append(operand.constraint.isVariadic() ? ", ArrayRef<Value *> "
                                                       : ", Value *");
      paramList.append(getArgumentName(op, numOperands));
      ++numOperands;
    } else {
      // TODO(antiagainst): Support default initializer for attributes
      const auto &namedAttr = op.getAttribute(numAttrs);
      const auto &attr = namedAttr.attr;
      paramList.append(", ");
      if (attr.isOptional())
        paramList.append("/*optional*/");
      paramList.append(
          (attr.getStorageType() + Twine(" ") + namedAttr.name).str());
      ++numAttrs;
    }
  }

  if (numOperands + numAttrs != op.getNumArgs())
    return PrintFatalError(
        "op arguments must be either operands or attributes");

  auto &method =
      opClass.newMethod("void", "build", paramList, OpMethod::MP_Static);

  bool hasVariadicOperand = op.hasVariadicOperand();

  // Push all result types to the result
  if (numResults > 0) {
    if (!useOperandType && !useAttrType) {
      bool hasVariadicResult = op.hasVariadicResult();
      int numNonVariadicResults =
          numResults - static_cast<int>(hasVariadicResult);

      if (numNonVariadicResults > 0) {
        method.body() << "  " << builderOpState << "->addTypes({"
                      << resultNames.front();
        for (int i = 1; i < numNonVariadicResults; ++i) {
          method.body() << ", " << resultNames[i];
        }
        method.body() << "});\n";
      }

      if (hasVariadicResult) {
        method.body() << "  " << builderOpState << "->addTypes("
                      << resultNames.back() << ");\n";
      }
    } else {
      std::string resultType;
      if (useAttrType) {
        const auto &namedAttr = op.getAttribute(0);
        if (namedAttr.attr.isTypeAttr()) {
          resultType = formatv("{0}.getValue()", namedAttr.name);
        } else {
          resultType = formatv("{0}.getType()", namedAttr.name);
        }
      } else {
        const char *index =
            (numOperands == 1 && hasVariadicOperand) ? ".front()" : "";
        resultType =
            formatv("{0}{1}->getType()", getArgumentName(op, 0), index).str();
      }
      method.body() << "  " << builderOpState << "->addTypes({" << resultType;
      for (unsigned i = 1; i != numResults; ++i)
        method.body() << ", " << resultType;
      method.body() << "});\n\n";
    }
  }

  // Push all operands to the result
  int numNonVariadicOperands =
      numOperands - static_cast<int>(hasVariadicOperand);
  if (numNonVariadicOperands > 0) {
    method.body() << "  " << builderOpState << "->addOperands({"
                  << getArgumentName(op, 0);
    for (int i = 1; i < numNonVariadicOperands; ++i) {
      method.body() << ", " << getArgumentName(op, i);
    }
    method.body() << "});\n";
  }
  if (hasVariadicOperand) {
    method.body() << "  " << builderOpState << "->addOperands("
                  << getArgumentName(op, numOperands - 1) << ");\n";
  }

  // Push all attributes to the result
  for (const auto &namedAttr : op.getAttributes()) {
    if (!namedAttr.attr.isDerivedAttr()) {
      bool emitNotNullCheck = namedAttr.attr.isOptional();
      if (emitNotNullCheck) {
        method.body() << formatv("  if ({0}) ", namedAttr.name) << "{\n";
      }
      method.body() << formatv("  {0}->addAttribute(\"{1}\", {1});\n",
                               builderOpState, namedAttr.name);
      if (emitNotNullCheck) {
        method.body() << "  }\n";
      }
    }
  }
}

void OpEmitter::genBuilder() {
  // Handle custom builders if provided.
  // TODO(antiagainst): Create wrapper class for OpBuilder to hide the native
  // TableGen API calls here.
  {
    auto *listInit = dyn_cast_or_null<ListInit>(def.getValueInit("builders"));
    if (listInit) {
      for (Init *init : listInit->getValues()) {
        Record *builderDef = cast<DefInit>(init)->getDef();
        StringRef params = builderDef->getValueAsString("params");
        StringRef body = builderDef->getValueAsString("body");
        bool hasBody = !body.empty();

        auto &method =
            opClass.newMethod("void", "build", params, OpMethod::MP_Static,
                              /*declOnly=*/!hasBody);
        if (hasBody)
          method.body() << body;
      }
    }
  }

  auto numResults = op.getNumResults();
  bool hasVariadicResult = op.hasVariadicResult();
  int numNonVariadicResults = numResults - int(hasVariadicResult);

  auto numOperands = op.getNumOperands();
  bool hasVariadicOperand = op.hasVariadicOperand();
  int numNonVariadicOperands = numOperands - int(hasVariadicOperand);

  // Generate default builders that requires all result type, operands, and
  // attributes as parameters.

  // We generate three builders here:
  // 1. one having a stand-alone parameter for each result type / operand /
  //    attribute, and
  // 2. one having an aggregated parameter for all result types / operands /
  //    attributes, and
  // 3. one having a stand-alone prameter for each operand and attribute,
  //    use the first operand's type as all result types
  // to facilitate different call patterns.

  // 1. Stand-alone parameters

  genStandaloneParamBuilder(/*useOperandType=*/false, /*useAttrType=*/false);

  // 2. Aggregated parameters

  // Signature
  std::string params =
      std::string("Builder *, OperationState *") + builderOpState +
      ", ArrayRef<Type> resultTypes, ArrayRef<Value *> operands, "
      "ArrayRef<NamedAttribute> attributes";
  auto &m = opClass.newMethod("void", "build", params, OpMethod::MP_Static);
  auto &body = m.body();

  // Result types
  if (!(hasVariadicResult && numNonVariadicResults == 0))
    body << "  assert(resultTypes.size()"
         << (hasVariadicResult ? " >= " : " == ") << numNonVariadicResults
         << "u && \"mismatched number of return types\");\n";
  body << "  " << builderOpState << "->addTypes(resultTypes);\n";

  // Operands
  if (!(hasVariadicOperand && numNonVariadicOperands == 0))
    body << "  assert(operands.size()" << (hasVariadicOperand ? " >= " : " == ")
         << numNonVariadicOperands
         << "u && \"mismatched number of parameters\");\n";
  body << "  " << builderOpState << "->addOperands(operands);\n\n";

  // Attributes
  body << "  for (const auto& pair : attributes)\n"
       << "    " << builderOpState
       << "->addAttribute(pair.first, pair.second);\n";

  // 3. Deduced result types

  bool useOperandType = op.hasTrait("SameOperandsAndResultType");
  bool useAttrType = op.hasTrait("FirstAttrDerivedResultType");
  if (!op.hasVariadicResult() && (useOperandType || useAttrType))
    genStandaloneParamBuilder(useOperandType, useAttrType);
}

void OpEmitter::genCanonicalizerDecls() {
  if (!def.getValueAsBit("hasCanonicalizer"))
    return;

  const char *const params =
      "OwningRewritePatternList &results, MLIRContext *context";
  opClass.newMethod("void", "getCanonicalizationPatterns", params,
                    OpMethod::MP_Static, /*declOnly=*/true);
}

void OpEmitter::genFolderDecls() {
  bool hasSingleResult = op.getNumResults() == 1;

  if (def.getValueAsBit("hasConstantFolder")) {
    if (hasSingleResult) {
      const char *const params =
          "ArrayRef<Attribute> operands, MLIRContext *context";
      opClass.newMethod("Attribute", "constantFold", params, OpMethod::MP_None,
                        /*declOnly=*/true);
    } else {
      const char *const params =
          "ArrayRef<Attribute> operands, SmallVectorImpl<Attribute> &results, "
          "MLIRContext *context";
      opClass.newMethod("LogicalResult", "constantFold", params,
                        OpMethod::MP_None, /*declOnly=*/true);
    }
  }

  if (def.getValueAsBit("hasFolder")) {
    if (hasSingleResult) {
      opClass.newMethod("Value *", "fold", /*params=*/"", OpMethod::MP_None,
                        /*declOnly=*/true);
    } else {
      opClass.newMethod("bool", "fold", "SmallVectorImpl<Value *> &results",
                        OpMethod::MP_None,
                        /*declOnly=*/true);
    }
  }
}

void OpEmitter::genParser() {
  if (!hasStringAttribute(def, "parser"))
    return;

  auto &method = opClass.newMethod(
      "bool", "parse", "OpAsmParser *parser, OperationState *result",
      OpMethod::MP_Static);
  auto parser = def.getValueAsString("parser").ltrim().rtrim(" \t\v\f\r");
  method.body() << "  " << parser;
}

void OpEmitter::genPrinter() {
  auto valueInit = def.getValueInit("printer");
  CodeInit *codeInit = dyn_cast<CodeInit>(valueInit);
  if (!codeInit)
    return;

  auto &method = opClass.newMethod("void", "print", "OpAsmPrinter *p");
  auto printer = codeInit->getValue().ltrim().rtrim(" \t\v\f\r");
  method.body() << "  " << printer;
}

void OpEmitter::genVerifier() {
  auto valueInit = def.getValueInit("verifier");
  CodeInit *codeInit = dyn_cast<CodeInit>(valueInit);
  bool hasCustomVerify = codeInit && !codeInit->getValue().empty();
  if (!hasCustomVerify && op.getNumArgs() == 0 && op.getNumResults() == 0 &&
      op.getNumPredOpTraits() == 0)
    return;

  auto &method = opClass.newMethod("LogicalResult", "verify", /*params=*/"");
  auto &body = method.body();
  FmtContext fctx;
  fctx.withOp("(*this->getOperation())");

  // Verify the attributes have the correct type.
  for (const auto &namedAttr : op.getAttributes()) {
    const auto &attr = namedAttr.attr;

    if (attr.isDerivedAttr())
      continue;

    auto attrName = namedAttr.name;
    // Prefix with `tblgen_` to avoid hiding the attribute accessor.
    auto varName = tblgenNamePrefix + attrName;
    body << formatv("  auto {0} = this->getAttr(\"{1}\");\n", varName,
                    attrName);

    bool allowMissingAttr =
        attr.hasDefaultValueInitializer() || attr.isOptional();
    if (allowMissingAttr) {
      // If the attribute has a default value, then only verify the predicate if
      // set. This does effectively assume that the default value is valid.
      // TODO: verify the debug value is valid (perhaps in debug mode only).
      body << "  if (" << varName << ") {\n";
    } else {
      body << "  if (!" << varName
           << ") return emitOpError(\"requires attribute '" << attrName
           << "'\");\n  {\n";
    }

    auto attrPred = attr.getPredicate();
    if (!attrPred.isNull()) {
      body << tgfmt("    if (!($0)) return emitOpError(\"attribute '$1' "
                    "failed to satisfy constraint: $2\");\n",
                    /*ctx=*/nullptr,
                    tgfmt(attrPred.getCondition(), &fctx.withSelf(varName)),
                    attrName, attr.getDescription());
    }

    body << "  }\n";
  }

  // Emits verification code for an operand or result.
  auto verifyValue = [&](const tblgen::NamedTypeConstraint &value, int index,
                         bool isOperand) -> void {
    // TODO: Handle variadic operand/result verification.
    if (value.constraint.isVariadic())
      return;

    // TODO: Commonality between matchers could be extracted to have a more
    // concise code.
    if (value.hasPredicate()) {
      auto description = value.constraint.getDescription();
      body << "  if (!("
           << tgfmt(value.constraint.getConditionTemplate(),
                    &fctx.withSelf("this->getOperation()->get" +
                                   Twine(isOperand ? "Operand" : "Result") +
                                   "(" + Twine(index) + ")->getType()"))
           << "))\n";
      body << "    return emitOpError(\"" << (isOperand ? "operand" : "result")
           << " #" << index
           << (description.empty() ? " type precondition failed"
                                   : " must be " + Twine(description))
           << "\");\n";
    }
  };

  for (unsigned i = 0, e = op.getNumOperands(); i < e; ++i) {
    verifyValue(op.getOperand(i), i, /*isOperand=*/true);
  }

  for (unsigned i = 0, e = op.getNumResults(); i < e; ++i) {
    verifyValue(op.getResult(i), i, /*isOperand=*/false);
  }

  for (auto &trait : op.getTraits()) {
    if (auto t = dyn_cast<tblgen::PredOpTrait>(&trait)) {
      body << tgfmt("  if (!($0))\n    return emitOpError(\""
                    "failed to verify that $1\");\n",
                    &fctx, tgfmt(t->getPredTemplate(), &fctx),
                    t->getDescription());
    }
  }

  if (hasCustomVerify)
    body << codeInit->getValue() << "\n";
  else
    body << "  return mlir::success();\n";
}

void OpEmitter::genTraits() {
  auto numResults = op.getNumResults();
  bool hasVariadicResult = op.hasVariadicResult();

  // Add return size trait.
  if (hasVariadicResult) {
    if (numResults == 1)
      opClass.addTrait("VariadicResults");
    else
      opClass.addTrait("AtLeastNResults<" + Twine(numResults - 1) + ">::Impl");
  } else {
    switch (numResults) {
    case 0:
      opClass.addTrait("ZeroResult");
      break;
    case 1:
      opClass.addTrait("OneResult");
      break;
    default:
      opClass.addTrait("NResults<" + Twine(numResults) + ">::Impl");
      break;
    }
  }

  for (const auto &trait : op.getTraits()) {
    if (auto opTrait = dyn_cast<tblgen::NativeOpTrait>(&trait))
      opClass.addTrait(opTrait->getTrait());
  }

  // Add variadic size trait and normal op traits.
  auto numOperands = op.getNumOperands();
  bool hasVariadicOperand = op.hasVariadicOperand();

  // Add operand size trait.
  if (hasVariadicOperand) {
    if (numOperands == 1)
      opClass.addTrait("VariadicOperands");
    else
      opClass.addTrait("AtLeastNOperands<" + Twine(numOperands - 1) +
                       ">::Impl");
  } else {
    opClass.addTrait("NOperands<" + Twine(numOperands) + ">::Impl");
  }
}

void OpEmitter::genOpNameGetter() {
  auto &method = opClass.newMethod("StringRef", "getOperationName",
                                   /*params=*/"", OpMethod::MP_Static);
  method.body() << "  return \"" << op.getOperationName() << "\";\n";
}

// Emits the opcode enum and op classes.
static void emitOpClasses(const std::vector<Record *> &defs, raw_ostream &os,
                          bool emitDecl) {
  IfDefScope scope("GET_OP_CLASSES", os);
  for (auto *def : defs) {
    if (emitDecl) {
      os << formatv(opCommentHeader,
                    Operator::getQualCppClassName(def->getName()),
                    "declarations");
      OpEmitter::emitDecl(*def, os);
    } else {
      os << formatv(opCommentHeader,
                    Operator::getQualCppClassName(def->getName()),
                    "definitions");
      OpEmitter::emitDef(*def, os);
    }
  }
}

// Emits a comma-separated list of the ops.
static void emitOpList(const std::vector<Record *> &defs, raw_ostream &os) {
  IfDefScope scope("GET_OP_LIST", os);

  interleave(
      defs,
      [&os](Record *def) {
        os << Operator::getQualCppClassName(def->getName());
      },
      [&os]() { os << ",\n"; });
}

static bool emitOpDecls(const RecordKeeper &recordKeeper, raw_ostream &os) {
  emitSourceFileHeader("Op Declarations", os);

  const auto &defs = recordKeeper.getAllDerivedDefinitions("Op");
  emitOpClasses(defs, os, /*emitDecl=*/true);

  return false;
}

static bool emitOpDefs(const RecordKeeper &recordKeeper, raw_ostream &os) {
  emitSourceFileHeader("Op Definitions", os);

  const auto &defs = recordKeeper.getAllDerivedDefinitions("Op");
  emitOpList(defs, os);
  emitOpClasses(defs, os, /*emitDecl=*/false);

  return false;
}

static mlir::GenRegistration
    genOpDecls("gen-op-decls", "Generate op declarations",
               [](const RecordKeeper &records, raw_ostream &os) {
                 return emitOpDecls(records, os);
               });

static mlir::GenRegistration genOpDefs("gen-op-defs", "Generate op definitions",
                                       [](const RecordKeeper &records,
                                          raw_ostream &os) {
                                         return emitOpDefs(records, os);
                                       });
