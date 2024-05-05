/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2019-2022 Aksel Alpay and contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef HIPSYCL_GLUE_JIT_HPP
#define HIPSYCL_GLUE_JIT_HPP

#include "hipSYCL/common/hcf_container.hpp"
#include "hipSYCL/common/debug.hpp"
#include "hipSYCL/common/small_map.hpp"
#include "hipSYCL/common/filesystem.hpp"
#include "hipSYCL/compiler/llvm-to-backend/LLVMToBackend.hpp"
#include "hipSYCL/runtime/error.hpp"
#include "hipSYCL/runtime/kernel_cache.hpp"
#include "hipSYCL/glue/kernel_configuration.hpp"
#include "hipSYCL/runtime/application.hpp"
#include <cstddef>
#include <vector>
#include <atomic>
#include <fstream>
#include <string>

#include "hipSYCL/common/hcf_container.hpp"
#include "hipSYCL/common/kernel_info.hpp"

namespace hipsycl {
namespace glue {
namespace jit {

inline bool readFile(const std::string& Filename, std::string& Out) {
  std::ifstream File{Filename, std::ios::binary|std::ios::ate};
  if(!File.is_open())
    return false;

  auto size = File.tellg();

  if (size == 0) {
      Out = std::string{};
      return true;
  }

  std::string result(size, '\0');

  File.seekg(0, std::ios::beg);
  File.read(result.data(), size);

  Out = result;

  return true;
}

// Map arguments passed to a kernel function on the C++ level to
// arguments of the kernel function. This is necessary because
// C++ arguments (especially structs) may have been decomposed
// into their elements in the kernel function prototype.
class cxx_argument_mapper {
public:
  cxx_argument_mapper(const rt::hcf_kernel_info &kernel_info, void **args,
                      const std::size_t *arg_sizes, std::size_t num_args,
                      const common::kernelinfo::KernelInfo& info = {"", ""}) {
    
    if (!info.filename.empty() && !info.kernel_name.empty()) {

      // Extract the new/modified kernel from external
      std::string InputFile = info.filename;
      std::string HcfInput;
      std::string source;

        // Read the HCF file
      if(!readFile(InputFile, HcfInput)) {
        std::cout << "Could not open file: " << InputFile << std::endl;
      }

      // Initialize the HCF container and perform some checks on the device image node
      common::hcf_container ext_hcf{HcfInput};
      auto* KernelNode = ext_hcf.root_node()->get_subnode("kernels");
      if(!KernelNode) {
        std::cout << "Invalid ext_hcf: Could not find 'kernels' node" << std::endl;
        return;
      }

      KernelNode = KernelNode->get_subnode(info.kernel_name);
      if(!KernelNode) {
        std::cout << "Invalid ext_hcf: Could not find specified kernel_name node" << std::endl;
         return;
      }

      // investigate parameters
      auto *parameters_node = KernelNode->get_subnode("parameters");
      if (!parameters_node)
          return;

      std::size_t num_subnodes = parameters_node->get_subnodes().size();
      
      for(int i = 0; i < num_subnodes; ++i) {

        const auto *param_info_node =
          parameters_node->get_subnode(std::to_string(i));

        if (!param_info_node)
          return;

        auto *byte_size = param_info_node->get_value("byte-size");
        auto *byte_offset = param_info_node->get_value("byte-offset");
        auto *original_index = param_info_node->get_value("original-index");

        if (!byte_size)
          return;
        if (!byte_offset)
          return;
          
        std::size_t arg_size = std::stoll(*byte_size);
        std::size_t arg_offset = std::stoll(*byte_offset);
        std::size_t arg_original_index = std::stoll(*original_index);

        assert(arg_original_index < num_args);

        void *data_ptr = add_offset(args[arg_original_index], arg_offset);
        
        if(!data_ptr)
          return;

        _mapped_data.push_back(data_ptr);
        _mapped_sizes.push_back(arg_size);

      }
    } else {

      std::size_t num_params = kernel_info.get_num_parameters();

      for(int i = 0; i < num_params; ++i) {
        std::size_t arg_size = kernel_info.get_argument_size(i);
        std::size_t arg_offset = kernel_info.get_argument_offset(i);
        std::size_t arg_original_index = kernel_info.get_original_argument_index(i);

        assert(arg_original_index < num_args);

        void *data_ptr = add_offset(args[arg_original_index], arg_offset);

        if(!data_ptr)
          return;

        _mapped_data.push_back(data_ptr);
        _mapped_sizes.push_back(arg_size);
      }

    }

    _mapping_result = true;
  }

  bool mapping_available() const {
    return _mapping_result;
  }

  void** get_mapped_args() {
    return _mapped_data.data();
  }

  void* const* get_mapped_args() const {
    return _mapped_data.data();
  }

  const std::size_t* get_mapped_arg_sizes() const {
    return _mapped_sizes.data();
  }

  std::size_t get_mapped_num_args() const {
    return _mapped_data.size();
  }
private:
  void *add_offset(void *ptr, std::size_t offset_bytes) const {
    return static_cast<void *>(static_cast<char *>(ptr) + offset_bytes);
  }

  bool _mapping_result = false;
  std::vector<void*> _mapped_data;
  std::vector<std::size_t> _mapped_sizes; 
};

class default_llvm_image_selector {
public:
  std::string operator()(const rt::hcf_kernel_info* kernel_info) const {
    return "llvm-ir.global";
  }
};

template <class ImageSelector = default_llvm_image_selector>
std::string select_image(const rt::hcf_kernel_info* kernel_info,
                         std::vector<std::string>* all_kernels_in_image_out,
                         const ImageSelector &sel = ImageSelector{}) {
  std::string image_name = sel(kernel_info);

  const rt::hcf_image_info *selected_image_info =
      rt::hcf_cache::get().get_image_info(kernel_info->get_hcf_object_id(),
                                          image_name);

  if (!selected_image_info)
    return {};

  if(all_kernels_in_image_out) {
    *all_kernels_in_image_out = selected_image_info->get_contained_kernels();
  }
  return image_name;
}

using symbol_list_t = compiler::LLVMToBackendTranslator::SymbolListType;

class runtime_linker {
  
public:
  using resolver = compiler::LLVMToBackendTranslator::ExternalSymbolResolver;
  using llvm_module_id = resolver::LLVMModuleId;


  runtime_linker(compiler::LLVMToBackendTranslator *translator,
                            const symbol_list_t &imported_symbol_names) {

    auto symbol_mapper = [this](const symbol_list_t& sl){ return this->map_smybols(sl); };

    auto bitcode_retriever = [this](llvm_module_id id,
                                    symbol_list_t &imported_symbols) {
      return this->retrieve_bitcode(id, imported_symbols);
    };

    translator->provideExternalSymbolResolver(
        resolver{symbol_mapper, bitcode_retriever, imported_symbol_names});
  }


private:

  std::vector<llvm_module_id> map_smybols(const symbol_list_t& sym_list) {
    std::vector<llvm_module_id> ir_modules_to_link;

    auto candidate_selector = [&, this](const std::string &symbol_name,
            const rt::hcf_cache::symbol_resolver_list &images) {
      for (const auto &img : images) {
        // Always attempt to link with global LLVM IR for now
        if (img.image_node->node_id == "llvm-ir.global") {
          _image_node_to_hcf_map[img.image_node] = img.hcf_id;
          ir_modules_to_link.push_back(
              reinterpret_cast<llvm_module_id>(img.image_node));
        } else {

          HIPSYCL_DEBUG_INFO << "jit::setup_linking: Discarding image "
                            << img.image_node->node_id << " @"
                            << img.image_node << " from HCF " << img.hcf_id
                            << "\n";
        }
      }
    };

    rt::hcf_cache::get().symbol_lookup(
        sym_list, candidate_selector);

    return ir_modules_to_link;
  }

  std::string retrieve_bitcode(llvm_module_id id, symbol_list_t& imported_symbols) const {

    const auto* hcf_image_node = reinterpret_cast<common::hcf_container::node*>(id);

    assert(_image_node_to_hcf_map.contains(hcf_image_node));

    auto v = _image_node_to_hcf_map.find(hcf_image_node);
    
    if(v == _image_node_to_hcf_map.end())
      return {};
    
    rt::hcf_object_id hcf_id = v->second;
    imported_symbols = hcf_image_node->get_as_list("imported-symbols");

    std::string bitcode;
    rt::hcf_cache::get().get_hcf(hcf_id)->get_binary_attachment(hcf_image_node, bitcode);

    return bitcode;
  }

  // This is used to map images to the owning HCF object ids.
  common::small_map<const common::hcf_container::node *, rt::hcf_object_id>
        _image_node_to_hcf_map;

};

inline rt::result compile(compiler::LLVMToBackendTranslator *translator,
                          const std::string &source,
                          const glue::kernel_configuration &config,
                          const symbol_list_t& imported_symbol_names,
                          std::string &output) {

  assert(translator);

  runtime_linker configure_linker {translator, imported_symbol_names};

  // Apply configuration
  translator->setS2IRConstant<sycl::jit::current_backend, int>(
      translator->getBackendId());
  for(const auto& entry : config.s2_ir_entries()) {
    translator->setS2IRConstant(entry.get_name(), entry.get_data_buffer());
  }
  if(translator->getKernels().size() == 1) {
    // Currently we only can specialize kernel arguments for the 
    // single-kernel code object model
    for(const auto& entry : config.specialized_arguments()) {
      translator->specializeKernelArgument(translator->getKernels().front(),
                                          entry.first, &entry.second);
    }
  }

  for(const auto& option : config.build_options()) {
    std::string option_name = glue::to_string(option.first);
    std::string option_value =
        option.second.int_value.has_value()
            ? std::to_string(option.second.int_value.value())
            : option.second.string_value.value();
    
    translator->setBuildOption(option_name, option_value);
  }

  for(const auto& flag : config.build_flags()) {
    translator->setBuildFlag(glue::to_string(flag));
  }
  
  // Transform code
  if(!translator->fullTransformation(source, output)) {
    // In case of failure, if a dump directory for IR is set,
    // dump the IR
    auto failure_dump_directory =
        rt::application::get_settings()
            .get<rt::setting::sscp_failed_ir_dump_directory>();
            
    if(!failure_dump_directory.empty()) {
      static std::atomic<std::size_t> failure_index = 0;
      std::string filename = common::filesystem::join_path(
          failure_dump_directory,
          "failed_ir_" + std::to_string(failure_index) + ".bc");
      
      std::ofstream out{filename.c_str(), std::ios::trunc|std::ios::binary};
      if(out.is_open()) {
        const std::string& failed_ir = translator->getFailedIR();
        out.write(failed_ir.c_str(), failed_ir.size());
      }

      ++failure_index;
    }
    
    return rt::make_error(__hipsycl_here(),
                      rt::error_info{"jit::compile: Encountered errors:\n" +
                                 translator->getErrorLogAsString()});
  }

  return rt::make_success();
}

inline rt::result compile(compiler::LLVMToBackendTranslator* translator,
                          const common::hcf_container* hcf,
                          const std::string& image_name,
                          const glue::kernel_configuration &config,
                          std::string &output,
                          const common::kernelinfo::KernelInfo& info = {"", ""}) {
  assert(hcf);
  assert(hcf->root_node());


  auto images_node = hcf->root_node()->get_subnode("images");
  if(!images_node) {
    return rt::make_error(
        __hipsycl_here(),
        rt::error_info{
            "jit::compile: Invalid HCF, no node named 'images' was found"});
  }

  auto target_image_node = images_node->get_subnode(image_name);
  if(!target_image_node) {
    return rt::make_error(__hipsycl_here(),
                          rt::error_info{"jit::compile: Requested image " +
                                         image_name +
                                         " was not defined in HCF"});
  }

  if(!target_image_node->has_binary_data_attached()) {
    return rt::make_error(
        __hipsycl_here(),
        rt::error_info{"jit::compile: Image " + image_name +
                       " was defined in HCF without data"});
  }
  std::string source;
  // If true, it's the module_custom_single or module_custom_parallel mode

  if (!info.filename.empty() && !info.kernel_name.empty()) {
    HIPSYCL_DEBUG_INFO << "jit::compile: In custom mode, extracting kernel "
                            << info.kernel_name << " from "
                            << info.filename << " HCF." << "\n";
    // Extract the new/modified kernel from external
    std::string InputFile = info.filename; //"add.hcf";
    std::string HcfInput;

    // Read the HCF file
    if(!readFile(InputFile, HcfInput)) {
      std::cout << "Could not open file: " << InputFile << std::endl;
    }

    // Initialize the HCF container and perform some checks on the device image node
    common::hcf_container ext_hcf{HcfInput};
    auto* ImgNode = ext_hcf.root_node()->get_subnode("images");
    if(!ImgNode) {
      // std::cout << "Invalid ext_hcf: Could not find 'images' node" << std::endl;
      return rt::make_error(
          __hipsycl_here(),
          rt::error_info{
              "jit::compile: Invalid ext_hcf: Could not find 'images' node"});
    }

    ImgNode = ImgNode->get_subnode(image_name);
    if(!ImgNode) {
      // std::cout << "Invalid ext_hcf: Could not find specified device image node" << std::endl;
      return rt::make_error(
          __hipsycl_here(),
          rt::error_info{
              "jit::compile: Could not find specified device image node"});
    }

    if(!ImgNode->has_binary_data_attached()){
      std::cout << "Invalid ext_hcf: Specified node has no data attached to it." << std::endl;
      return rt::make_error(
          __hipsycl_here(),
          rt::error_info{
              "jit::compile: Specified node has no data attached to it"});
    }

    // Extract the binary content of Image node
    if(!ext_hcf.get_binary_attachment(ImgNode, source)){
      return rt::make_error(
          __hipsycl_here(),
          rt::error_info{
              "jit::compile: Could not extract binary data for ext_hcf image " +
              image_name});
    }
  } else {
    if(!hcf->get_binary_attachment(target_image_node, source)) {
    return rt::make_error(
        __hipsycl_here(),
        rt::error_info{
            "jit::compile: Could not extract binary data for HCF image " +
            image_name});
    }
  }

  symbol_list_t imported_symbol_names =
      target_image_node->get_as_list("imported-symbols");

  return compile(translator, source, config, imported_symbol_names, output);
}

inline rt::result compile(compiler::LLVMToBackendTranslator* translator,
                          rt::hcf_object_id hcf_object,
                          const std::string& image_name,
                          const glue::kernel_configuration &config,
                          std::string &output,
                          const common::kernelinfo::KernelInfo& info = {"", ""}) {

  const common::hcf_container* hcf = rt::hcf_cache::get().get_hcf(hcf_object);
  if(!hcf) {
    return rt::make_error(
        __hipsycl_here(),
        rt::error_info{"jit::compile: Could not obtain HCF object"});
  }

  return compile(translator, hcf, image_name, config,
                 output, info);
}

}
}
}

#endif
