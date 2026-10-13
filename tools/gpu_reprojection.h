#pragma once
#include "camera_reproject_spirv.h"
namespace dxvk_capture {
// Uses the existing DXVK queue, with diagnostic uploads/waits. No game image writes.
class ReprojectCompute {
  Capture& c;
  VkBuffer buffers[2]{};VkDeviceMemory memory[2]{};void* maps[2]{};
  VkShaderModule shader{};VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};
  VkPipeline pipeline{};VkDescriptorPool descriptors{};
#define FN(name) PFN_##name name=nullptr
  FN(vkCreateShaderModule);FN(vkDestroyShaderModule);FN(vkCreateDescriptorSetLayout);FN(vkDestroyDescriptorSetLayout);
  FN(vkCreatePipelineLayout);FN(vkDestroyPipelineLayout);FN(vkCreateComputePipelines);FN(vkDestroyPipeline);
  FN(vkCreateDescriptorPool);FN(vkDestroyDescriptorPool);FN(vkAllocateDescriptorSets);FN(vkUpdateDescriptorSets);
  FN(vkCmdBindPipeline);FN(vkCmdBindDescriptorSets);FN(vkCmdPushConstants);FN(vkCmdDispatch);FN(vkFlushMappedMemoryRanges);
#undef FN
public:
  explicit ReprojectCompute(Capture& capture):c(capture) {
    HMODULE loader=GetModuleHandleW(L"vulkan-1.dll");
    auto instanceProc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
    if(!instanceProc)throw std::runtime_error("Compute loader entry missing");
    auto get=reinterpret_cast<PFN_vkGetDeviceProcAddr>(instanceProc(c.instance,"vkGetDeviceProcAddr"));
#define LOAD(name) name=reinterpret_cast<PFN_##name>(get(c.device,#name));if(!name)throw std::runtime_error(#name " missing")
    LOAD(vkCreateShaderModule);LOAD(vkDestroyShaderModule);LOAD(vkCreateDescriptorSetLayout);LOAD(vkDestroyDescriptorSetLayout);
    LOAD(vkCreatePipelineLayout);LOAD(vkDestroyPipelineLayout);LOAD(vkCreateComputePipelines);LOAD(vkDestroyPipeline);
    LOAD(vkCreateDescriptorPool);LOAD(vkDestroyDescriptorPool);LOAD(vkAllocateDescriptorSets);LOAD(vkUpdateDescriptorSets);
    LOAD(vkCmdBindPipeline);LOAD(vkCmdBindDescriptorSets);LOAD(vkCmdPushConstants);LOAD(vkCmdDispatch);LOAD(vkFlushMappedMemoryRanges);
#undef LOAD
  }
  ~ReprojectCompute() {
    if(c.pending){c.interop->LockSubmissionQueue();c.vkQueueWaitIdle(c.queue);c.interop->ReleaseSubmissionQueue();c.pending=false;}
    if(pipeline)vkDestroyPipeline(c.device,pipeline,nullptr);
    if(layout)vkDestroyPipelineLayout(c.device,layout,nullptr);
    if(descriptors)vkDestroyDescriptorPool(c.device,descriptors,nullptr);
    if(setLayout)vkDestroyDescriptorSetLayout(c.device,setLayout,nullptr);
    if(shader)vkDestroyShaderModule(c.device,shader,nullptr);
    for(int i=0;i<2;i++){if(maps[i])c.vkUnmapMemory(c.device,memory[i]);if(buffers[i])c.vkDestroyBuffer(c.device,buffers[i],nullptr);if(memory[i])c.vkFreeMemory(c.device,memory[i],nullptr);}
  }
  struct Push { std::array<float,16> transform;std::array<uint32_t,4> sizes;std::array<uint32_t,4> sampling; };
  std::vector<float> run(const std::vector<float>& input,const Push& push) {
    static_assert(sizeof(Push)==96,"Compute ABI");
    const size_t n=size_t(push.sizes[0])*push.sizes[1];
    if(!n||n>1024*1024||input.size()!=n*10)throw std::runtime_error("Compute sample budget/size mismatch");
    VkQueueFamilyProperties familyProps{};
    auto getInstance=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),"vkGetInstanceProcAddr"));
    auto getFamilies=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(getInstance(c.instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
    uint32_t count=0;getFamilies(c.physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);getFamilies(c.physical,&count,families.data());
    if(c.family>=count)throw std::runtime_error("Compute queue family unavailable");
    familyProps=families[c.family];if(!(familyProps.queueFlags&VK_QUEUE_COMPUTE_BIT))throw std::runtime_error("DXVK submission queue cannot compute");
    const VkDeviceSize sizes[2]={VkDeviceSize(input.size()*4),VkDeviceSize(n*8*4)};
    for(int i=0;i<2;i++) {
      if(sizes[i]>c.physicalProperties.limits.maxStorageBufferRange)throw std::runtime_error("Compute storage range exceeded");
      VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=sizes[i];b.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
      vkcheck(c.vkCreateBuffer(c.device,&b,nullptr,&buffers[i]),"Compute buffer");VkMemoryRequirements req{};c.vkGetBufferMemoryRequirements(c.device,buffers[i],&req);memory[i]=c.allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      vkcheck(c.vkBindBufferMemory(c.device,buffers[i],memory[i],0),"Compute bind");vkcheck(c.vkMapMemory(c.device,memory[i],0,VK_WHOLE_SIZE,0,&maps[i]),"Compute map");
    }
    std::memcpy(maps[0],input.data(),size_t(sizes[0]));
    VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=memory[0];range.size=VK_WHOLE_SIZE;vkcheck(vkFlushMappedMemoryRanges(c.device,1,&range),"Compute flush");
    VkShaderModuleCreateInfo sm{};sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;sm.codeSize=sizeof(cameraReprojectSpirv);sm.pCode=cameraReprojectSpirv;vkcheck(vkCreateShaderModule(c.device,&sm,nullptr,&shader),"Compute shader");
    std::array<VkDescriptorSetLayoutBinding,2> bindings{};
    for(uint32_t i=0;i<2;i++){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo sl{};sl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;sl.bindingCount=2;sl.pBindings=bindings.data();vkcheck(vkCreateDescriptorSetLayout(c.device,&sl,nullptr,&setLayout),"Compute set layout");
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;vkcheck(vkCreatePipelineLayout(c.device,&pl,nullptr,&layout),"Compute pipeline layout");
    VkComputePipelineCreateInfo cp{};cp.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;cp.layout=layout;cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cp.stage.module=shader;cp.stage.pName="main";vkcheck(vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline),"Compute pipeline");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&ps;vkcheck(vkCreateDescriptorPool(c.device,&dp,nullptr,&descriptors),"Compute descriptor pool");
    VkDescriptorSetAllocateInfo da{};da.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;da.descriptorPool=descriptors;da.descriptorSetCount=1;da.pSetLayouts=&setLayout;VkDescriptorSet set{};vkcheck(vkAllocateDescriptorSets(c.device,&da,&set),"Compute descriptor set");
    std::array<VkDescriptorBufferInfo,2> bi{};std::array<VkWriteDescriptorSet,2> writes{};
    for(uint32_t i=0;i<2;i++){bi[i]={buffers[i],0,sizes[i]};writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&bi[i];}vkUpdateDescriptorSets(c.device,2,writes.data(),0,nullptr);
    vkcheck(c.vkResetCommandPool(c.device,c.pool,0),"Compute reset pool");vkcheck(c.vkResetFences(c.device,1,&c.fence),"Compute reset fence");VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vkcheck(c.vkBeginCommandBuffer(c.command,&begin),"Compute begin");
    VkBufferMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=buffers[0];barrier.size=VK_WHOLE_SIZE;
    c.vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
    vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);vkCmdPushConstants(c.command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push),&push);vkCmdDispatch(c.command,(push.sizes[0]+7)/8,(push.sizes[1]+7)/8,1);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;barrier.buffer=buffers[1];c.vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
    vkcheck(c.vkEndCommandBuffer(c.command),"Compute end");VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&c.command;c.interop->LockSubmissionQueue();VkResult result=c.vkQueueSubmit(c.queue,1,&submit,c.fence);c.interop->ReleaseSubmissionQueue();vkcheck(result,"Compute submit");c.pending=true;vkcheck(c.vkWaitForFences(c.device,1,&c.fence,VK_TRUE,5'000'000'000ull),"Compute wait");c.pending=false;
    range.memory=memory[1];vkcheck(c.vkInvalidateMappedMemoryRanges(c.device,1,&range),"Compute invalidate");std::vector<float> output(n*8);std::memcpy(output.data(),maps[1],size_t(sizes[1]));return output;
  }
};
}
