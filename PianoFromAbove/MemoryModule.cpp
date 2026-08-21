#include "MemoryModule.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef IMAGE_SIZEOF_BASE_RELOCATION
#define IMAGE_SIZEOF_BASE_RELOCATION (sizeof(IMAGE_BASE_RELOCATION))
#endif

typedef BOOL (WINAPI *DllEntryProc)(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved);

struct MEMORYMODULE {
    PIMAGE_NT_HEADERS headers;
    unsigned char *codeBase;
    HMEMORYMODULE *modules;
    int numModules;
    BOOL initialized;
    BOOL isDLL;
    CustomLoadLibraryFunc loadLibrary;
    CustomGetProcAddressFunc getProcAddress;
    CustomFreeLibraryFunc freeLibrary;
    void *userdata;
#ifdef _WIN64
    PRUNTIME_FUNCTION exceptionTable;
#endif
};

static HMEMORYMODULE DefaultLoadLibrary(const char *filename, void *userdata) {
    (void)userdata;
    return (HMEMORYMODULE)LoadLibraryA(filename);
}

static FARPROC DefaultGetProcAddress(HMEMORYMODULE module, const char *name, void *userdata) {
    (void)userdata;
    return GetProcAddress((HMODULE)module, name);
}

static void DefaultFreeLibrary(HMEMORYMODULE module, void *userdata) {
    (void)userdata;
    FreeLibrary((HMODULE)module);
}

static BOOL CopySections(const unsigned char *data, size_t size, PIMAGE_NT_HEADERS old_headers, MEMORYMODULE *module) {
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(module->headers);
    for (WORD i = 0; i < module->headers->FileHeader.NumberOfSections; i++, section++) {
        if (section->SizeOfRawData == 0) {
            // Section contains uninitialized data
            DWORD sectionSize = old_headers->OptionalHeader.SectionAlignment;
            if (sectionSize > 0) {
                unsigned char *dest = (unsigned char *)VirtualAlloc(
                    module->codeBase + section->VirtualAddress,
                    sectionSize,
                    MEM_COMMIT,
                    PAGE_READWRITE
                );
                if (dest == NULL) return FALSE;
                dest = module->codeBase + section->VirtualAddress;
                section->Misc.PhysicalAddress = (DWORD)((uintptr_t)dest & 0xffffffff);
                memset(dest, 0, sectionSize);
            }
            continue;
        }

        if (size < (size_t)section->PointerToRawData + section->SizeOfRawData) {
            return FALSE;
        }

        unsigned char *dest = (unsigned char *)VirtualAlloc(
            module->codeBase + section->VirtualAddress,
            section->SizeOfRawData,
            MEM_COMMIT,
            PAGE_READWRITE
        );
        if (dest == NULL) return FALSE;

        dest = module->codeBase + section->VirtualAddress;
        memcpy(dest, data + section->PointerToRawData, section->SizeOfRawData);
        section->Misc.PhysicalAddress = (DWORD)((uintptr_t)dest & 0xffffffff);
    }
    return TRUE;
}

static BOOL PerformBaseRelocation(MEMORYMODULE *module, ptrdiff_t delta) {
    DWORD relocVA = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
    DWORD relocSize = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
    if (relocVA == 0 || relocSize == 0) {
        return (delta == 0);
    }

    PIMAGE_BASE_RELOCATION relocation = (PIMAGE_BASE_RELOCATION)(module->codeBase + relocVA);
    while (relocation->VirtualAddress > 0) {
        unsigned char *dest = module->codeBase + relocation->VirtualAddress;
        unsigned short *relInfo = (unsigned short *)((unsigned char *)relocation + IMAGE_SIZEOF_BASE_RELOCATION);
        DWORD count = (relocation->SizeOfBlock - IMAGE_SIZEOF_BASE_RELOCATION) / sizeof(unsigned short);

        for (DWORD i = 0; i < count; i++, relInfo++) {
            int type = *relInfo >> 12;
            int offset = *relInfo & 0xfff;

            switch (type) {
            case IMAGE_REL_BASED_ABSOLUTE:
                break;
            case IMAGE_REL_BASED_HIGHLOW: {
                DWORD *patchAddr32 = (DWORD *)(dest + offset);
                *patchAddr32 += (DWORD)delta;
                break;
            }
#ifdef _WIN64
            case IMAGE_REL_BASED_DIR64: {
                ULONGLONG *patchAddr64 = (ULONGLONG *)(dest + offset);
                *patchAddr64 += (ULONGLONG)delta;
                break;
            }
#endif
            default:
                break;
            }
        }

        relocation = (PIMAGE_BASE_RELOCATION)((unsigned char *)relocation + relocation->SizeOfBlock);
    }
    return TRUE;
}

static BOOL BuildImportTable(MEMORYMODULE *module) {
    DWORD importVA = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    DWORD importSize = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (importVA == 0 || importSize == 0) {
        return TRUE;
    }

    PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR)(module->codeBase + importVA);
    for (; importDesc->Name; importDesc++) {
        const char *dllName = (const char *)(module->codeBase + importDesc->Name);
        HMEMORYMODULE handle = module->loadLibrary(dllName, module->userdata);
        if (handle == NULL) {
            return FALSE;
        }

        HMEMORYMODULE *newModules = (HMEMORYMODULE *)realloc(module->modules, (module->numModules + 1) * sizeof(HMEMORYMODULE));
        if (newModules == NULL) {
            module->freeLibrary(handle, module->userdata);
            return FALSE;
        }
        module->modules = newModules;
        module->modules[module->numModules++] = handle;

        PIMAGE_THUNK_DATA thunkRef;
        PIMAGE_THUNK_DATA funcRef;

        if (importDesc->OriginalFirstThunk) {
            thunkRef = (PIMAGE_THUNK_DATA)(module->codeBase + importDesc->OriginalFirstThunk);
            funcRef = (PIMAGE_THUNK_DATA)(module->codeBase + importDesc->FirstThunk);
        } else {
            thunkRef = (PIMAGE_THUNK_DATA)(module->codeBase + importDesc->FirstThunk);
            funcRef = (PIMAGE_THUNK_DATA)(module->codeBase + importDesc->FirstThunk);
        }

        for (; thunkRef->u1.AddressOfData; thunkRef++, funcRef++) {
            if (IMAGE_SNAP_BY_ORDINAL(thunkRef->u1.Ordinal)) {
                funcRef->u1.Function = (uintptr_t)module->getProcAddress(handle, (const char *)IMAGE_ORDINAL(thunkRef->u1.Ordinal), module->userdata);
            } else {
                PIMAGE_IMPORT_BY_NAME thunkData = (PIMAGE_IMPORT_BY_NAME)(module->codeBase + thunkRef->u1.AddressOfData);
                funcRef->u1.Function = (uintptr_t)module->getProcAddress(handle, (const char *)thunkData->Name, module->userdata);
            }
            if (funcRef->u1.Function == 0) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static BOOL FinalizeSections(MEMORYMODULE *module) {
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(module->headers);
    for (WORD i = 0; i < module->headers->FileHeader.NumberOfSections; i++, section++) {
        DWORD protect = 0;
        DWORD oldProtect = 0;
        BOOL executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        BOOL readable   = (section->Characteristics & IMAGE_SCN_MEM_READ) != 0;
        BOOL writable   = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;

        if (section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) {
            VirtualFree(module->codeBase + section->VirtualAddress, section->SizeOfRawData, MEM_DECOMMIT);
            continue;
        }

        if (executable) {
            if (writable) {
                protect = PAGE_EXECUTE_READWRITE;
            } else if (readable) {
                protect = PAGE_EXECUTE_READ;
            } else {
                protect = PAGE_EXECUTE;
            }
        } else {
            if (writable) {
                protect = PAGE_READWRITE;
            } else if (readable) {
                protect = PAGE_READONLY;
            } else {
                protect = PAGE_NOACCESS;
            }
        }

        if (section->Characteristics & IMAGE_SCN_MEM_NOT_CACHED) {
            protect |= PAGE_NOCACHE;
        }

        DWORD size = section->SizeOfRawData;
        if (size == 0) {
            if (section->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) {
                size = module->headers->OptionalHeader.SizeOfInitializedData;
            } else if (section->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) {
                size = module->headers->OptionalHeader.SizeOfUninitializedData;
            }
        }

        if (size > 0) {
            if (!VirtualProtect(module->codeBase + section->VirtualAddress, size, protect, &oldProtect)) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static void ExecuteTLS(MEMORYMODULE *module, DWORD reason) {
    DWORD tlsVA = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress;
    if (tlsVA == 0) return;

    PIMAGE_TLS_DIRECTORY tls = (PIMAGE_TLS_DIRECTORY)(module->codeBase + tlsVA);
    PIMAGE_TLS_CALLBACK *callback = (PIMAGE_TLS_CALLBACK *)tls->AddressOfCallBacks;
    if (callback) {
        while (*callback) {
            (*callback)((PVOID)module->codeBase, reason, NULL);
            callback++;
        }
    }
}

HMEMORYMODULE MemoryLoadLibraryEx(
    const void *data,
    size_t size,
    CustomLoadLibraryFunc loadLibrary,
    CustomGetProcAddressFunc getProcAddress,
    CustomFreeLibraryFunc freeLibrary,
    void *userdata
) {
    if (data == NULL || size < sizeof(IMAGE_DOS_HEADER)) {
        return NULL;
    }

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)data;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return NULL;
    }

    if (size < (size_t)dosHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS)) {
        return NULL;
    }

    PIMAGE_NT_HEADERS old_headers = (PIMAGE_NT_HEADERS)((const unsigned char *)data + dosHeader->e_lfanew);
    if (old_headers->Signature != IMAGE_NT_SIGNATURE) {
        return NULL;
    }

#ifdef _WIN64
    if (old_headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        return NULL;
    }
#else
    if (old_headers->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        return NULL;
    }
#endif

    if ((old_headers->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0) {
        // Not a DLL
        return NULL;
    }

    MEMORYMODULE *module = (MEMORYMODULE *)calloc(1, sizeof(MEMORYMODULE));
    if (module == NULL) {
        return NULL;
    }

    module->isDLL = TRUE;
    module->loadLibrary = loadLibrary ? loadLibrary : DefaultLoadLibrary;
    module->getProcAddress = getProcAddress ? getProcAddress : DefaultGetProcAddress;
    module->freeLibrary = freeLibrary ? freeLibrary : DefaultFreeLibrary;
    module->userdata = userdata;

    // Reserve memory for image
    unsigned char *codeBase = (unsigned char *)VirtualAlloc(
        (LPVOID)(old_headers->OptionalHeader.ImageBase),
        old_headers->OptionalHeader.SizeOfImage,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );

    if (codeBase == NULL) {
        // Try allocating anywhere
        codeBase = (unsigned char *)VirtualAlloc(
            NULL,
            old_headers->OptionalHeader.SizeOfImage,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
    }

    if (codeBase == NULL) {
        free(module);
        return NULL;
    }

    module->codeBase = codeBase;

    // Commit memory for headers
    unsigned char *headers = (unsigned char *)VirtualAlloc(
        codeBase,
        old_headers->OptionalHeader.SizeOfHeaders,
        MEM_COMMIT,
        PAGE_READWRITE
    );
    if (headers == NULL) {
        VirtualFree(codeBase, 0, MEM_RELEASE);
        free(module);
        return NULL;
    }

    memcpy(headers, dosHeader, old_headers->OptionalHeader.SizeOfHeaders);
    module->headers = (PIMAGE_NT_HEADERS)&((const unsigned char *)headers)[dosHeader->e_lfanew];
    module->headers->OptionalHeader.ImageBase = (uintptr_t)codeBase;

    // Copy sections
    if (!CopySections((const unsigned char *)data, size, old_headers, module)) {
        printf("CopySections failed\n");
        MemoryFreeLibrary((HMEMORYMODULE)module);
        return NULL;
    }

    // Base relocations
    ptrdiff_t locationDelta = (ptrdiff_t)(codeBase - old_headers->OptionalHeader.ImageBase);
    if (locationDelta != 0) {
        if (!PerformBaseRelocation(module, locationDelta)) {
            printf("PerformBaseRelocation failed\n");
            MemoryFreeLibrary((HMEMORYMODULE)module);
            return NULL;
        }
    }

    // Build import table
    if (!BuildImportTable(module)) {
        printf("BuildImportTable failed\n");
        MemoryFreeLibrary((HMEMORYMODULE)module);
        return NULL;
    }

    // Give whole image PAGE_EXECUTE_READWRITE so packed DLLs (like Petite/UPX) can unpack themselves in DllMain
    DWORD oldProtect = 0;
    VirtualProtect(module->codeBase, module->headers->OptionalHeader.SizeOfImage, PAGE_EXECUTE_READWRITE, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), module->codeBase, module->headers->OptionalHeader.SizeOfImage);

#ifdef _WIN64
    // Register exception table for x64 SEH
    DWORD exVA = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
    DWORD exSize = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size;
    if (exVA != 0 && exSize != 0) {
        printf("Registering exception table (%u entries)...\n", (unsigned)(exSize / sizeof(RUNTIME_FUNCTION))); fflush(stdout);
        module->exceptionTable = (PRUNTIME_FUNCTION)(module->codeBase + exVA);
        DWORD count = exSize / sizeof(RUNTIME_FUNCTION);
        RtlAddFunctionTable(module->exceptionTable, count, (DWORD64)module->codeBase);
    }
#endif

    // Run TLS callbacks
    printf("Executing TLS callbacks...\n"); fflush(stdout);
    ExecuteTLS(module, DLL_PROCESS_ATTACH);

    // Call DllMain
    if (module->headers->OptionalHeader.AddressOfEntryPoint != 0) {
        printf("Calling DllMain at %p...\n", (void*)(module->codeBase + module->headers->OptionalHeader.AddressOfEntryPoint)); fflush(stdout);
        DllEntryProc DllEntry = (DllEntryProc)(module->codeBase + module->headers->OptionalHeader.AddressOfEntryPoint);
        BOOL success = (*DllEntry)((HINSTANCE)module->codeBase, DLL_PROCESS_ATTACH, NULL);
        if (!success) {
            printf("DllMain returned FALSE\n"); fflush(stdout);
            MemoryFreeLibrary((HMEMORYMODULE)module);
            return NULL;
        }
        module->initialized = TRUE;
    }

    printf("Calling FinalizeSections after DllMain...\n"); fflush(stdout);
    // Finalize section protections after unpacker has executed
    FinalizeSections(module);
    FlushInstructionCache(GetCurrentProcess(), module->codeBase, module->headers->OptionalHeader.SizeOfImage);
    printf("MemoryLoadLibraryEx completed successfully!\n"); fflush(stdout);

    return (HMEMORYMODULE)module;
}

HMEMORYMODULE MemoryLoadLibrary(const void *data, size_t size) {
    return MemoryLoadLibraryEx(data, size, NULL, NULL, NULL, NULL);
}

static FARPROC ResolveForwardedExport(const char *forwarder, CustomLoadLibraryFunc loadLibrary, CustomGetProcAddressFunc getProcAddress, void *userdata) {
    char dllName[256];
    const char *funcName = strchr(forwarder, '.');
    if (!funcName) return NULL;

    size_t dllLen = funcName - forwarder;
    if (dllLen >= sizeof(dllName) - 5) return NULL;

    memcpy(dllName, forwarder, dllLen);
    dllName[dllLen] = '\0';
    strcat_s(dllName, sizeof(dllName), ".dll");
    funcName++; // Skip '.'

    HMEMORYMODULE hMod = loadLibrary ? loadLibrary(dllName, userdata) : (HMEMORYMODULE)LoadLibraryA(dllName);
    if (!hMod) return NULL;

    if (*funcName == '#') {
        WORD ord = (WORD)atoi(funcName + 1);
        return getProcAddress ? getProcAddress(hMod, (const char *)(uintptr_t)ord, userdata) : GetProcAddress((HMODULE)hMod, MAKEINTRESOURCEA(ord));
    }
    return getProcAddress ? getProcAddress(hMod, funcName, userdata) : GetProcAddress((HMODULE)hMod, funcName);
}

FARPROC MemoryGetProcAddress(HMEMORYMODULE mod, const char *name) {
    if (mod == NULL || name == NULL) {
        return NULL;
    }

    MEMORYMODULE *module = (MEMORYMODULE *)mod;

    // Check if name is an ordinal
    if ((uintptr_t)name <= 0xFFFF) {
        return MemoryGetProcAddressOrdinal(mod, (WORD)(uintptr_t)name);
    }

    DWORD exportVA = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    DWORD exportSize = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (exportVA == 0 || exportSize == 0) {
        return NULL;
    }

    PIMAGE_EXPORT_DIRECTORY exports = (PIMAGE_EXPORT_DIRECTORY)(module->codeBase + exportVA);
    if (exports->NumberOfNames == 0 || exports->NumberOfFunctions == 0) {
        return NULL;
    }

    DWORD *nameAddresses = (DWORD *)(module->codeBase + exports->AddressOfNames);
    WORD *nameOrdinals = (WORD *)(module->codeBase + exports->AddressOfNameOrdinals);
    DWORD *functions = (DWORD *)(module->codeBase + exports->AddressOfFunctions);

    // Search for function by name
    int low = 0;
    int high = exports->NumberOfNames - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        const char *curName = (const char *)(module->codeBase + nameAddresses[mid]);
        int cmp = strcmp(name, curName);
        if (cmp == 0) {
            WORD ordinal = nameOrdinals[mid];
            if (ordinal >= exports->NumberOfFunctions) {
                return NULL;
            }
            DWORD funcRVA = functions[ordinal];
            // Check for forwarded export
            if (funcRVA >= exportVA && funcRVA < exportVA + exportSize) {
                const char *forwarder = (const char *)(module->codeBase + funcRVA);
                return ResolveForwardedExport(forwarder, module->loadLibrary, module->getProcAddress, module->userdata);
            }
            return (FARPROC)(module->codeBase + funcRVA);
        } else if (cmp < 0) {
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }

    return NULL;
}

FARPROC MemoryGetProcAddressOrdinal(HMEMORYMODULE mod, WORD ordinal) {
    if (mod == NULL) {
        return NULL;
    }

    MEMORYMODULE *module = (MEMORYMODULE *)mod;
    DWORD exportVA = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    DWORD exportSize = module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (exportVA == 0 || exportSize == 0) {
        return NULL;
    }

    PIMAGE_EXPORT_DIRECTORY exports = (PIMAGE_EXPORT_DIRECTORY)(module->codeBase + exportVA);
    if (ordinal < exports->Base || (DWORD)(ordinal - exports->Base) >= exports->NumberOfFunctions) {
        return NULL;
    }

    DWORD *functions = (DWORD *)(module->codeBase + exports->AddressOfFunctions);
    DWORD funcRVA = functions[ordinal - exports->Base];
    if (funcRVA == 0) {
        return NULL;
    }

    if (funcRVA >= exportVA && funcRVA < exportVA + exportSize) {
        const char *forwarder = (const char *)(module->codeBase + funcRVA);
        return ResolveForwardedExport(forwarder, module->loadLibrary, module->getProcAddress, module->userdata);
    }

    return (FARPROC)(module->codeBase + funcRVA);
}

void MemoryFreeLibrary(HMEMORYMODULE mod) {
    if (mod == NULL) return;

    MEMORYMODULE *module = (MEMORYMODULE *)mod;

    if (module->initialized) {
        if (module->headers->OptionalHeader.AddressOfEntryPoint != 0) {
            DllEntryProc DllEntry = (DllEntryProc)(module->codeBase + module->headers->OptionalHeader.AddressOfEntryPoint);
            (*DllEntry)((HINSTANCE)module->codeBase, DLL_PROCESS_DETACH, NULL);
        }
        ExecuteTLS(module, DLL_PROCESS_DETACH);
        module->initialized = FALSE;
    }

#ifdef _WIN64
    if (module->exceptionTable) {
        RtlDeleteFunctionTable(module->exceptionTable);
        module->exceptionTable = NULL;
    }
#endif

    if (module->modules != NULL) {
        for (int i = 0; i < module->numModules; i++) {
            if (module->modules[i] != NULL) {
                module->freeLibrary(module->modules[i], module->userdata);
            }
        }
        free(module->modules);
    }

    if (module->codeBase != NULL) {
        VirtualFree(module->codeBase, 0, MEM_RELEASE);
    }

    free(module);
}
