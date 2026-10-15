// Compile only. The compiler, rather than a source-line count, selects slots for
// inherited methods and overloaded virtuals. These are SDK candidates, not game
// signatures or permission to call an unverified game object.
#include <materialsystem/imaterialsystem.h>
#include <tier1/callqueue.h>

extern "C" __declspec(dllexport) ICallQueue* candidateGetCallQueue(IMatRenderContext* context) {
  return context->GetCallQueue();
}
extern "C" __declspec(dllexport) IMatRenderContext* candidateGetRenderContext(IMaterialSystem* system) {
  return system->GetRenderContext();
}
extern "C" __declspec(dllexport) void candidateGetMatrix4(IMatRenderContext* context, VMatrix* matrix) {
  context->GetMatrix(MATERIAL_VIEW, matrix);
}
extern "C" __declspec(dllexport) void candidateGetMatrix3(IMatRenderContext* context, matrix3x4_t* matrix) {
  context->GetMatrix(MATERIAL_VIEW, matrix);
}
