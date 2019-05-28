/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "HBCParser.h"
#include "ProfileAnalyzer.h"
#include "StructuredPrinter.h"

#include "hermes/BCGen/HBC/BytecodeDisassembler.h"
#include "hermes/Public/Buffer.h"
#include "hermes/SourceMap/SourceMapGenerator.h"
#include "hermes/Support/MemoryBuffer.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <map>
#include <sstream>
#include <string>

using namespace hermes;
using namespace hermes::hbc;

using llvm::raw_fd_ostream;

static llvm::cl::opt<std::string> InputFilename(
    llvm::cl::desc("input file"),
    llvm::cl::Positional);

static llvm::cl::opt<std::string> DumpOutputFilename(
    "out",
    llvm::cl::desc("Output file name"));

static llvm::cl::opt<std::string> StartupCommands(
    "c",
    llvm::cl::desc(
        "A list of commands to execute before entering "
        "interactive mode separated by semicolon. "
        "You can use this option to execute a bunch of commands "
        "without entering interactive mode, like -c \"cmd1;cmd2;quit\""));

static llvm::cl::opt<bool> PrettyDisassemble(
    "pretty-disassemble",
    llvm::cl::init(true),
    llvm::cl::desc("Pretty print the disassembled bytecode(true by default)"));

static llvm::cl::opt<std::string> AnalyzeMode(
    "mode",
    llvm::cl::desc(
        "The analysis mode you want to use(either instruction or function)"));

static llvm::cl::opt<std::string> ProfileFile(
    "profile-file",
    llvm::cl::desc(
        "Log file in json format generated by basic block profiler"));

static llvm::cl::opt<bool> ShowSectionRanges(
    "show-section-ranges",
    llvm::cl::init(false),
    llvm::cl::desc("Show the byte range of each section in bytecode"));

static llvm::cl::opt<bool> HumanizeSectionRanges(
    "human",
    llvm::cl::init(false),
    llvm::cl::desc("Print bytecode section ranges in hex format"));

static bool executeCommand(
    llvm::raw_ostream &os,
    ProfileAnalyzer &analyzer,
    BytecodeDisassembler &disassembler,
    const std::string &commandWithOptions);

/// Wrapper around std::getline().
/// Read a line from cin, storing it into \p line.
/// \return true if we have a line, false if input was exhausted.
static bool getline(std::string &line) {
  for (;;) {
    // On receiving EINTR, getline() in libc++ appears to incorrectly mark
    // cin's EOF bit. This means that sucessive getline() calls will return
    // EOF. Workaround this iostream bug by clearing the cin flags on EINTR.
    errno = 0;
    if (std::getline(std::cin, line)) {
      return true;
    } else if (errno == EINTR) {
      std::cin.clear();
    } else {
      // Input exhausted.
      return false;
    }
  }
}

static void printHelp(llvm::Optional<llvm::StringRef> command = llvm::None) {
  // Declare variables for help text.
  static const std::unordered_map<std::string, std::string> commandToHelpText = {
      {"function",
       "'function': Compute the runtime instruction frequency "
       "for each function and display in desceding order."
       "Each function name is displayed together with its source code line number .\n"
       "'function <FUNC_ID>': Dump basic block stats for function with id <FUNC_ID>.\n\n"
       "USAGE: function <FUNC_ID>\n"
       "       func <FUNC_ID>\n"},
      {"instruction",
       "Computes the runtime instruction frequency for each instruction"
       "and displays it in descending order.\n\n"
       "USAGE: instruction\n"
       "       inst\n"},
      {"disassemble",
       "'disassemble': Display bytecode disassembled output of whole binary.\n"
       "'disassemble <FUNC_ID>': Display bytecode disassembled output of function with id <FUNC_ID>.\n"
       "Add the '-offsets' flag to show virtual offsets for all instructions.\n\n"
       "USAGE: disassemble <FUNC_ID> [-offsets]\n"
       "       dis <FUNC_ID> [-offsets]\n"},
      {"summary",
       "Display overall summary information.\n\n"
       "USAGE: summary\n"},
      {"io",
       "Visualize function page I/O access working set"
       "in basic block profile trace.\n\n"
       "USAGE: io\n"},
      {"block",
       "Display top hot basic blocks in sorted order.\n\n"
       "USAGE: block\n"},
      {"at-virtual",
       "Display information about the function at a given virtual offset.\n\n"
       "USAGE: at-virtual <OFFSET> [-json]\n"},
      {"help",
       "Help instructions for hbcdump tool commands.\n\n"
       "USAGE: help <COMMAND>\n"
       "       h <COMMAND>\n"},
  };

  if (command.hasValue() && !command->empty()) {
    const auto it = commandToHelpText.find(*command);
    if (it == commandToHelpText.end()) {
      llvm::outs() << "Invalid command: " << *command << '\n';
      return;
    }
    llvm::outs() << it->second;
  } else {
    static const std::string topLevelHelpText =
        "These commands are defined internally. Type `help' to see this list.\n"
        "Type `help name' to find out more about the function `name'.\n\n";
    llvm::outs() << topLevelHelpText;
    for (const auto it : commandToHelpText) {
      llvm::outs() << it.first << '\n';
    }
  }
}

/// Enters interactive command loop.
static void enterCommandLoop(
    llvm::raw_ostream &os,
    std::shared_ptr<hbc::BCProvider> bcProvider,
    llvm::Optional<std::unique_ptr<llvm::MemoryBuffer>> profileBufferOpt,
    const std::vector<std::string> &startupCommands) {
  BytecodeDisassembler disassembler(bcProvider);

  // Include source information and func IDs by default in disassembly output.
  DisassemblyOptions options = DisassemblyOptions::IncludeSource |
      DisassemblyOptions::IncludeFunctionIds;
  if (PrettyDisassemble) {
    options = options | DisassemblyOptions::Pretty;
  }
  disassembler.setOptions(options);
  ProfileAnalyzer analyzer(
      os,
      bcProvider,
      profileBufferOpt.hasValue()
          ? llvm::Optional<std::unique_ptr<llvm::MemoryBuffer>>(
                std::move(profileBufferOpt.getValue()))
          : llvm::None);

  // Process startup commands.
  bool terminateLoop = false;
  for (const auto &command : startupCommands) {
    if (executeCommand(os, analyzer, disassembler, command)) {
      terminateLoop = true;
    }
  }

  while (!terminateLoop) {
    os << "hbcdump> ";
    std::string line;
    if (!getline(line)) {
      break;
    }
    terminateLoop = executeCommand(os, analyzer, disassembler, line);
  }
}

/// Find the first instance of a value in a container and remove it.
/// \return true if the value was found and removed, false otherwise.
template <typename Container, typename Value>
static bool findAndRemoveOne(Container &haystack, const Value &needle) {
  auto it = std::find(haystack.begin(), haystack.end(), needle);
  if (it != haystack.end()) {
    haystack.erase(it);
    return true;
  }
  return false;
}

/// Simple RAII helper for setting and reverting disassembler options.
class DisassemblerOptionsHolder {
 public:
  DisassemblerOptionsHolder(
      BytecodeDisassembler &disassembler,
      DisassemblyOptions newOptions)
      : disassembler_(disassembler), savedOptions_(disassembler.getOptions()) {
    disassembler_.setOptions(newOptions);
  }

  ~DisassemblerOptionsHolder() {
    disassembler_.setOptions(savedOptions_);
  }

 private:
  BytecodeDisassembler &disassembler_;
  DisassemblyOptions savedOptions_;
};

/// Execute a single command from \p commandTokens.
/// \return true telling caller to terminate the interactive command loop.
static bool executeCommand(
    llvm::raw_ostream &os,
    ProfileAnalyzer &analyzer,
    BytecodeDisassembler &disassembler,
    const std::string &commandWithOptions) {
  // Parse command tokens.
  llvm::SmallVector<llvm::StringRef, 8> commandTokens;
  llvm::StringRef(commandWithOptions).split(commandTokens, ' ');
  if (commandTokens.empty()) {
    // Ignore empty input.
    return false;
  }

  const llvm::StringRef command = commandTokens[0];
  if (command == "function" || command == "fun") {
    if (commandTokens.size() == 1) {
      analyzer.dumpFunctionStats();
    } else if (commandTokens.size() == 2) {
      uint32_t funcId;
      if (commandTokens[1].getAsInteger(0, funcId)) {
        os << "Error: cannot parse func_id as integer.\n";
        return false;
      }
      analyzer.dumpFunctionBasicBlockStat(funcId);
    } else {
      printHelp(command);
      return false;
    }
  } else if (command == "instruction" || command == "inst") {
    if (commandTokens.size() == 1) {
      analyzer.dumpInstructionStats();
    } else {
      printHelp(command);
      return false;
    }
  } else if (command == "disassemble" || command == "dis") {
    auto localOptions = findAndRemoveOne(commandTokens, "-offsets")
        ? DisassemblyOptions::IncludeVirtualOffsets
        : DisassemblyOptions::None;
    DisassemblerOptionsHolder optionsHolder(
        disassembler, disassembler.getOptions() | localOptions);
    if (commandTokens.size() == 1) {
      disassembler.disassemble(os);
    } else if (commandTokens.size() == 2) {
      uint32_t funcId;
      if (commandTokens[1].getAsInteger(0, funcId)) {
        os << "Error: cannot parse func_id as integer.\n";
        return false;
      }
      disassembler.disassembleFunction(funcId, os);
    } else {
      printHelp(command);
      return false;
    }
  } else if (command == "string" || command == "str") {
    uint32_t stringId;
    if (commandTokens[1].getAsInteger(0, stringId)) {
      os << "Error: cannot parse string_id as integer.\n";
      return false;
    }
    analyzer.dumpString(stringId);
  } else if (command == "filename") {
    uint32_t filenameId;
    if (commandTokens[1].getAsInteger(0, filenameId)) {
      os << "Error: cannot parse filename_id as integer.\n";
      return false;
    }
    analyzer.dumpFileName(filenameId);
  } else if (command == "offset" || command == "offsets") {
    bool json = findAndRemoveOne(commandTokens, "-json");
    std::unique_ptr<StructuredPrinter> printer =
        StructuredPrinter::create(os, json);
    if (commandTokens.size() == 1) {
      analyzer.dumpAllFunctionOffsets(*printer);
    } else if (commandTokens.size() == 2) {
      uint32_t funcId;
      if (commandTokens[1].getAsInteger(0, funcId)) {
        os << "Error: cannot parse func_id as integer.\n";
        return false;
      }
      analyzer.dumpFunctionOffsets(funcId, *printer);
    } else {
      os << "Usage: offsets [funcId]\n";
    }
  } else if (command == "io") {
    analyzer.dumpIO();
  } else if (command == "summary" || command == "sum") {
    analyzer.dumpSummary();
  } else if (command == "block") {
    analyzer.dumpBasicBlockStats();
  } else if (command == "at_virtual" || command == "at-virtual") {
    bool json = findAndRemoveOne(commandTokens, "-json");
    std::unique_ptr<StructuredPrinter> printer =
        StructuredPrinter::create(os, json);
    if (commandTokens.size() == 2) {
      uint32_t virtualOffset;
      if (commandTokens[1].getAsInteger(0, virtualOffset)) {
        os << "Error: cannot parse virtualOffset as integer.\n";
        return false;
      }
      auto funcId = analyzer.getFunctionFromVirtualOffset(virtualOffset);
      if (funcId.hasValue()) {
        analyzer.dumpFunctionOffsets(*funcId, *printer);
      } else {
        os << "Virtual offset " << virtualOffset << " is invalid.\n";
      }
    } else {
      printHelp(command);
      return false;
    }
  } else if (command == "epilogue" || command == "epi") {
    analyzer.dumpEpilogue();
  } else if (command == "help" || command == "h") {
    // Interactive help command.
    if (commandTokens.size() == 2) {
      printHelp(commandTokens[1]);
    } else {
      printHelp();
    }
    return false;
  } else if (command == "quit") {
    // Quit command loop.
    return true;
  } else {
    printHelp(command);
    return false;
  }
  os << "\n";
  return false;
}

int main(int argc, char **argv) {
  // Normalize the arg vector.
  llvm::InitLLVM initLLVM(argc, argv);
  llvm::sys::PrintStackTraceOnErrorSignal("hbcdump");
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::llvm_shutdown_obj Y;
  llvm::cl::ParseCommandLineOptions(argc, argv, "Hermes bytecode dump tool\n");

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileBufOrErr =
      llvm::MemoryBuffer::getFile(InputFilename);

  if (!fileBufOrErr) {
    llvm::errs() << "Error: fail to open file: " << InputFilename << ": "
                 << fileBufOrErr.getError().message() << "\n";
    return -1;
  }

  auto buffer =
      llvm::make_unique<hermes::MemoryBuffer>(fileBufOrErr.get().get());
  const uint8_t *bytecodeStart = buffer->data();
  auto ret =
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(std::move(buffer));
  if (!ret.first) {
    llvm::errs() << "Error: fail to deserializing bytecode: " << ret.second;
    return 1;
  }

  // Parse startup commands list(separated by semicolon).
  std::vector<std::string> startupCommands;
  if (!StartupCommands.empty()) {
    std::istringstream iss(StartupCommands.data());
    std::string command;
    while (getline(iss, command, ';')) {
      startupCommands.emplace_back(command);
    }
  }

  llvm::Optional<raw_fd_ostream> fileOS;
  if (!DumpOutputFilename.empty()) {
    std::error_code EC;
    fileOS.emplace(DumpOutputFilename.data(), EC, llvm::sys::fs::F_Text);
    if (EC) {
      llvm::errs() << "Error: fail to open file " << DumpOutputFilename << ": "
                   << EC.message() << '\n';
      return -1;
    }
  }
  auto &output = fileOS ? *fileOS : llvm::outs();

  if (ProfileFile.empty()) {
    if (ShowSectionRanges) {
      BytecodeSectionWalker walker(bytecodeStart, std::move(ret.first), output);
      walker.printSectionRanges(HumanizeSectionRanges);
    } else {
      enterCommandLoop(
          output, std::move(ret.first), llvm::None, startupCommands);
    }
  } else {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> profileBuffer =
        llvm::MemoryBuffer::getFile(ProfileFile);
    if (!profileBuffer) {
      llvm::errs() << "Error: fail to open file: " << ProfileFile
                   << profileBuffer.getError().message() << "\n";
      return -1;
    }
    enterCommandLoop(
        output,
        std::move(ret.first),
        std::move(profileBuffer.get()),
        startupCommands);
  }

  return 0;
}
