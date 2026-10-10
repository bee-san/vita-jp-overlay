/* Host boundary only: preserve the SDK's five virtual methods before its
 * destructor so production memory.cpp exercises its real slot-4 dispatch. */
#ifndef VJO_TEST_PAF_HEAP_ALLOCATOR_H
#define VJO_TEST_PAF_HEAP_ALLOCATOR_H
#include <cstddef>
#include <cstdint>
extern "C" void *GetGlobalHeapAllocator(void);
namespace paf { namespace memory {
enum AllocationBlockType { AllocationBlockType_User = 0 };
class MemoryAllocator {
public:
    virtual uint32_t GetType() const = 0;
    virtual AllocationBlockType GetBlockType() const = 0;
    virtual char *GetName() const = 0;
    virtual void GetRange(void **base, size_t *bytes) const = 0;
    virtual size_t GetFreeSize() = 0;
    virtual ~MemoryAllocator() {}
};
class HeapAllocator : public MemoryAllocator {
public:
    uint32_t GetType() const override { return 1; }
    AllocationBlockType GetBlockType() const override { return AllocationBlockType_User; }
    char *GetName() const override { return nullptr; }
    void GetRange(void **base, size_t *bytes) const override { *base = nullptr; *bytes = 0; }
    size_t GetFreeSize() override;
};
inline HeapAllocator *GetGlobalHeapAllocator()
{ return static_cast<HeapAllocator *>(::GetGlobalHeapAllocator()); }
}}
#endif
