
// Copyright (C) Force67 2019

#include <utl/file.h>
#include <utl/mem.h>

#include "runtime/code_lift.h"
#include "runtime/vprx/vprx.h"

#include "module.h"
#include "proc.h"

namespace krnl {
smodule::smodule(proc *process) : process(process) {
  /*-1 = no tls used*/
  info.handle = -1;
  info.tlsSlot = -1;

  /*set size from process config*/
  info.ripZoneSize = process->getEnv().ripZoneSize;
}

bool smodule::fromFile(const std::string &path) {
  utl::File file(path);
  if (!file.IsOpen())
    return false;

  ELFHeader diskHeader{};
  file.Read(diskHeader);

  if (diskHeader.magic == ELF_MAGIC &&
      diskHeader.machine == ELF_MACHINE_X86_64) {
    file.Seek(0, utl::seekMode::seek_set);

    data = std::make_unique<uint8_t[]>(file.GetSize());
    file.Read(data.get(), file.GetSize());
    return fromMem(std::move(data));
  }

  return false;
}

bool smodule::fromMem(std::unique_ptr<uint8_t[]> data) {
  /*TODO: figure out a way of getting rid of the back buffer*/
  this->data = std::move(data);

  elf = getOffset<ELFHeader>(0);
  segments = getOffset<ELFPgHeader>(elf->phoff);

  if (!mapImage()) {
    LOG_ERROR("smodule: Failed to map image");
    return false;
  }

  digestDynamic();

#ifdef _DEBUG
  LOG_TRACE("mapped {} at {}", info.name, fmt::ptr(info.base));
#endif
  setupTLS();

  if (!isDynlib()) {
    auto *seg = getSegment(ElfSegType::PT_SCE_PROCPARAM);
    if (seg) {
      info.procParam = getAddress<uint8_t>(seg->vaddr);
      info.procParamSize = seg->filesz;
    }
  }

  installEHFrame();

  if (elf->entry == 0)
    info.entry = nullptr;
  else
    info.entry = getAddress<uint8_t>(elf->entry);

  for (auto &it : sharedObjects) {
    if (it != "libSceGnmDriver_padebug")
      process->loadModule(it);
  }

  return true;
}

bool smodule::unload() {
  data.reset();

  // todo: unload memory from VMA
  return true;
}

void smodule::digestDynamic() {
  const auto *dynS = getSegment(ElfSegType::PT_DYNAMIC);
  const auto *dyldS = getSegment(ElfSegType::PT_SCE_DYNLIBDATA);

  uint8_t *dynldPtr = getOffset<uint8_t>(dyldS->offset);
  uint8_t *dynldAddr = getAddress<uint8_t>(dyldS->vaddr);
  // std::printf("addr = %p\n", dynldAddr);
  ELFDyn *dynamics = getOffset<ELFDyn>(dynS->offset);
  for (int32_t i = 0; i < (dynS->filesz / sizeof(ELFDyn)); i++) {
    auto *d = &dynamics[i];

    switch (d->tag) {
    case DT_SCE_HASH:
      hashes = reinterpret_cast<uint8_t *>(dynldPtr + d->un.value);
      break;
    case DT_INIT:
      info.initAddr = reinterpret_cast<uint8_t *>(dynldAddr + d->un.ptr);
      break;
    case DT_FINI:
      info.finiAddr = reinterpret_cast<uint8_t *>(dynldAddr + d->un.ptr);
      break;
    case DT_SCE_JMPREL:
      jmpslots = (ElfRel *)(dynldPtr + d->un.ptr);
      break;
    case DT_PLTRELSZ:
    case DT_SCE_PLTRELSZ:
      numJmpSlots = static_cast<uint32_t>(d->un.value / sizeof(ElfRel));
      break;
    case DT_SCE_STRTAB:
      strtab.ptr = (char *)(dynldPtr + d->un.ptr);
      break;
    case DT_STRSZ:
    case DT_SCE_STRSIZE:
      strtab.size = d->un.value;
      break;
    case DT_SCE_SYMTAB:
      symbols = reinterpret_cast<ElfSym *>(dynldPtr + d->un.ptr);
      break;
    case DT_SCE_SYMTABSZ:
      numSymbols = static_cast<uint32_t>(d->un.value / sizeof(ElfSym));
      break;
    case DT_SCE_RELA:
      rela = reinterpret_cast<ElfRel *>(dynldPtr + d->un.ptr);
      break;
    case DT_NEEDED: {
      auto name = (const char *)(strtab.ptr + (d->un.value & 0xFFFFFFFF));
      if (name) {
        std::string xname = name;
        /*quick but (valid?) hack for determining if an object is exported*/
        auto pos = xname.find(".prx");
        if (pos != -1) {
          sharedObjects.push_back(xname.substr(0, pos));
        }
      }

      break;
    }
    case DT_RELASZ:
    case DT_SCE_RELASZ:
      numRela = static_cast<uint32_t>(d->un.value / sizeof(ElfRel));
      break;
    case DT_SCE_EXPLIB:
    case DT_SCE_IMPLIB: {
      /*for now, in the future we also want to store explibs*/
      auto &e = impLibs.emplace_back();
      e.id = d->un.value >> 48;
      e.exported = d->tag == DT_SCE_EXPLIB;
      e.name = (const char *)(strtab.ptr + (d->un.value & 0xFFFFFFFF));
      break;
    }
    case DT_SCE_EXPORT_LIB_ATTR:
    case DT_SCE_IMPORT_LIB_ATTR: {
      uint16_t id = d->un.value >> 48;
      uint16_t idx = d->un.value & 0xFFF;

      for (auto &mod : impLibs) {
        if (mod.id == id) {
          mod.attr = idx;
          break;
        }
      }
      break;
    }
    case DT_SCE_NEEDED_MODULE: {
      auto &e = impModules.emplace_back();
      e.id = d->un.value >> 48;
      e.name = (const char *)(strtab.ptr + (d->un.value & 0xFFFFFFFF));
      break;
    }
    case DT_SCE_MODULE_ATTR: {
      uint16_t id = d->un.value >> 48;
      uint16_t idx = d->un.value & 0xFFF;

      for (auto &mod : impModules) {
        if (mod.id == id) {
          mod.attr = idx;
          break;
        }
      }
      break;
    }
    case DT_SCE_MODULEINFO:
      info.name = (const char *)(strtab.ptr + (d->un.value & 0xFFFFFFFF));
      break;
    case DT_SCE_FINGERPRINT:
      std::memcpy(info.fingerprint, getOffset<void>(d->un.value), 20);
      break;
    }
  }
}

bool smodule::mapImage() {
  // calculate total aligned code size
  uint32_t codeSize = 0;
  for (uint16_t i = 0; i < elf->phnum; ++i) {
    const auto *p = &segments[i];
    if (p->type == PT_LOAD || p->type == PT_SCE_RELRO) {
      codeSize += align_up(p->memsz, p->align);
    }
  }

  // could also check if INTERP exists
  if (codeSize == 0)
    return false;

  // reserve a region from xxxxxxxx00000000 - xxxxxxxxFFFFFFFF
  constexpr size_t one_mb = 1024ull * 1024ull;
  constexpr size_t eight_gb = 8ull * 1024ull * one_mb;

  info.base = static_cast<uint8_t *>(utl::allocMem(
      nullptr, eight_gb, utl::pageProtection::w, utl::allocationType::reserve));

  if (!info.base)
    return false;

  // immediately take module memory + rip Zone memory
  utl::allocMem(info.base, codeSize + info.ripZoneSize, utl::pageProtection::w,
                utl::allocationType::commit);

  info.codeSize = codeSize;
  info.ripZone = info.base + codeSize;

  std::memset(info.ripZone, 0xCC, info.ripZoneSize);
  utl::protectMem(info.ripZone, info.ripZoneSize, utl::pageProtection::rwx);

  // step 0: map data
  for (uint16_t i = 0; i < elf->phnum; i++) {
    const auto *s = &segments[i];
    if (s->type == PT_LOAD || s->type == PT_SCE_RELRO) {
      void *target = elf->type == ET_SCE_EXEC
                         ? reinterpret_cast<void *>(s->vaddr)
                         : getAddress<void>(s->paddr);

      auto *seg = s->flags & PF_X ? &info.textSeg : &info.dataSeg;
      seg->addr = static_cast<uint8_t *>(target);
      seg->size = s->memsz;

      std::memcpy(target, getOffset<void>(s->offset), s->filesz);
    }
  }

  // step 1: lift code
  for (uint16_t i = 0; i < elf->phnum; i++) {
    const auto *s = &segments[i];
    uint32_t perm = s->flags & (PF_R | PF_W | PF_X);
    if (s->type == PT_LOAD && perm == (PF_R | PF_X)) {
      runtime::codeLift lift(info.ripZone);
      LOG_ASSERT(lift.init());

      /*TODO: we should really introduce a cache here*/
      lift.transform(getAddress<uint8_t>(s->vaddr), s->filesz);
    }
  }

#if 1
  // temp hack: raise 5.05 kernel debug msg level
  if (info.handle == 1) {
    *getAddress<uint32_t>(0x68264) = UINT32_MAX;
    LOG_WARNING("Enabling libkernel debug messages");
  }
#endif

  // step 2: apply page protections
  for (uint16_t i = 0; i < elf->phnum; i++) {
    const auto *s = &segments[i];
    if (s->type == PT_LOAD) {
      uint32_t perm = s->flags & (PF_R | PF_W | PF_X);
      auto trans_perm = [](uint32_t op) {
        switch (op) {
        case (PF_R | PF_X):
          return utl::pageProtection::rx;
        case (PF_R | PF_W):
          return utl::pageProtection::w;
        case (PF_R):
          return utl::pageProtection::r;
        default:
          return utl::pageProtection::priv;
          /*todo: invalid parameter bugcheck*/
        }
      };

      utl::protectMem(getAddress<void>(s->vaddr), s->filesz, trans_perm(perm));
    }
  }

  return true;
}

bool smodule::setupTLS() {
  auto *p = getSegment(PT_TLS);
  if (p) {

    info.tlsAddr = getAddress<uint8_t>(p->vaddr);
    info.tlsalign = p->align;
    info.tlsSizeFile = p->filesz;
    info.tlsSizeMem = p->memsz;
    info.tlsSlot = process->nextFreeTLS();
  }

  return true;
}

static bool decodeNid(const char *name, uint64_t &lid, uint64_t &mid) {
  /*nid's are always 16 bytes long so we should get away
   with hard coding the access offset*/

  bool decodeSuccess = true;
  if (!runtime::decode_nid(&name[12], 1, lid))
    decodeSuccess = false;
  if (!runtime::decode_nid(&name[14], 1, mid))
    decodeSuccess = false;
  if (!decodeSuccess) {
    LOG_ERROR("decodeNid: can't decode symbol");
    return false;
  }
  return decodeSuccess;
}

bool smodule::resolveObfSymbol(const char *name, uintptr_t &ptrOut) {
  uint64_t libid = 0, modid = 0;
  if (!decodeNid(name, libid, modid))
    __debugbreak();

  const char *libname = nullptr;

  // TODO: could be done nicer
  for (auto &mod : impLibs) {
    if (mod.id == static_cast<int32_t>(libid)) {
      libname = mod.name;
      break;
    }
  }

  if (!libname)
    return false;

  for (auto &mod : impModules) {
    if (mod.id == static_cast<int32_t>(modid)) {
      auto xmod = process->getModule(mod.name);
      if (!xmod) {
        LOG_ERROR("resolveObfSymbol: Unknown module {} ({}) requestd", mod.name,
                  mod.id);
        return false;
      }

      char nameenc[12]{}; // name + null terminator
      std::strncpy(nameenc, name, 11);

      std::string longName =
          std::string(nameenc) + "#" + libname + "#" + mod.name;
      ptrOut = xmod->getSymbolFullName(longName.c_str());
      return true;
    }
  }

  return false;
}

/*invoked by sys_dynlib_process_needed_and_relocate*/
bool smodule::resolveImports() {
  /*unpatched functioncall*/
  uintptr_t addrBadCall =
      process->getModule("libkernel")
          ->getSymbolFullName("M0z6Dr6TNnM#libkernel#libkernel");

  for (uint32_t i = 0; i < numJmpSlots; i++) {
    const auto *r = &jmpslots[i];

    int32_t type = ELF64_R_TYPE(r->info);
    int32_t isym = ELF64_R_SYM(r->info);

    ElfSym *sym = &symbols[isym];

    if (type != R_X86_64_JUMP_SLOT) {
      LOG_WARNING("resolveImports: bad jump slot {}", i);
      continue;
    }

    if ((uint32_t)isym >= numSymbols || sym->st_name >= strtab.size) {
      LOG_WARNING("resolveImports: bad symbol index {} for relocation {}", isym,
                  i);
      continue;
    }

    int32_t binding = ELF64_ST_BIND(sym->st_info);
    if (binding == STB_LOCAL) {
      *getAddress<uintptr_t>(r->offset) =
          getAddressNPTR<uintptr_t>(sym->st_value);
      continue;
    }

    uintptr_t addr = 0;
    const char *name = &strtab.ptr[sym->st_name];

    if (!resolveObfSymbol(name, addr)) {
      return false;
    }

    if (!addr)
      addr = addrBadCall;

    *getAddress<uintptr_t>(r->offset) = addr;
  }

  return true;
}

/*invoked by sys_dynlib_process_needed_and_relocate*/
bool smodule::applyRelocations() {
  for (size_t i = 0; i < numRela; i++) {
    auto *r = &rela[i];

    uint32_t isym = ELF64_R_SYM(r->info);
    int32_t type = ELF64_R_TYPE(r->info);

    ElfSym *sym = &symbols[isym];
    int32_t bind = ELF64_ST_BIND(sym->st_info);

    if (isym >= numSymbols) {
      LOG_ERROR("Invalid symbol index {}", isym);
      continue;
    }

    uint64_t symVal = 0;

    if (bind == STB_LOCAL)
      symVal = sym->st_value;
    else if (bind == STB_GLOBAL || bind == STB_WEAK) {
      /*relative offset*/ // TODO (force): should we check MID here?
      if (sym->st_value)
        symVal = getAddressNPTR<uintptr_t>(sym->st_value);
      else {
        const char *name = &strtab.ptr[sym->st_name];

        if (!resolveObfSymbol(name, symVal)) {
          return false;
        }

        if (!symVal)
          __debugbreak();
      }
    }

    switch (type) {
    case R_X86_64_64:
      *getAddress<uint64_t>(r->offset) = symVal + r->addend;
      break;
    case R_X86_64_RELATIVE: /* base + ofs*/
      *getAddress<int64_t>(r->offset) = getAddressNPTR<int64_t>(r->addend);
      break;
    case R_X86_64_GLOB_DAT:
      *getAddress<uint64_t>(r->offset) = symVal;
      break;
    case R_X86_64_PC32:
      *getAddress<uint32_t>(r->offset) = static_cast<uint32_t>(
          symVal + r->addend - getAddressNPTR<uint64_t>(r->offset));
      break;
    case R_X86_64_DTPMOD64:
      *getAddress<uint64_t>(r->offset) += info.tlsSlot;
      break;
    case R_X86_64_DTPOFF32:
      *getAddress<uint32_t>(r->offset) +=
          static_cast<uint32_t>(symVal + r->addend);
      break;
    case R_X86_64_DTPOFF64:
      *getAddress<uint64_t>(r->offset) += symVal + r->addend;
      break;
    case R_X86_64_NONE:
      break;
    default:
      continue;
    }
  }

  return true;
}

uintptr_t smodule::getSymbol(uint64_t nid) {
  // are there any overrides for me?
  auto imp = runtime::vprx_get(info.name.c_str(), nid);
  if (imp != 0)
    return imp;

  for (uint32_t i = 0; i < numSymbols; i++) {
    const auto *s = &symbols[i];

    if (!s->st_value)
      continue;

    // if the symbol is exported
    // int32_t binding = ELF64_ST_BIND(s->st_info);

    const char *name = &strtab.ptr[s->st_name];

    uint64_t hid = 0;
    if (!runtime::decode_nid(name, 11, hid)) {
      LOG_ERROR("resolveExport: cant handle NID");
      return 0;
    }

    if (nid == hid) {
      return getAddressNPTR<uintptr_t>(s->st_value);
    }
  }

  return 0;
}

uintptr_t smodule::getSymbolFullName(const char *name) {
  // TODO: fix elf hash lookup

  auto elfHash = [](const char *name) {
    auto p = (const uint8_t *)name;
    uint32_t h = 0;
    uint32_t g;
    while (*p != '\0') {
      h = (h << 4) + *p++;
      if ((g = h & 0xF0000000ull) != 0) {
        h ^= g >> 24;
      }
      h &= ~g;
    }
    return h;
  };

  auto hash = elfHash(name);

  auto *htab = reinterpret_cast<uint32_t *>(hashes);
  uint32_t nbucket = htab[0];
  uint32_t nchain = htab[1];
  uint32_t *bucket = &htab[2];
  uint32_t *chain = &bucket[nbucket];

  /*char nameOut[11]{};
  runtime::encode_nid("module_start", reinterpret_cast<uint8_t*>(&nameOut));*/

  for (uint32_t i = bucket[hash % nbucket]; i; i = chain[i]) {
    const auto *s = &symbols[i];

    if (i > nchain)
      return 0;

    if (!s->st_value)
      continue;

    const char *sname = &strtab.ptr[s->st_name];
    if (std::strncmp(sname, name, 11) == 0) {
      return getAddressNPTR<uintptr_t>(s->st_value);
    }
  }

  return 0;
}

uintptr_t smodule::getSymbol2(const char *name) {
  for (uint32_t i = 0; i < numSymbols; i++) {
    const auto *s = &symbols[i];

    if (!s->st_value)
      continue;

    const char *sname = &strtab.ptr[s->st_name];

    if (std::strcmp(sname, name) == 0) {
      return getAddressNPTR<uintptr_t>(s->st_value);
    }
  }

  return 0;
}

// taken from idc's "uplift" project
void smodule::installEHFrame() {
  const auto *p = getSegment(PT_GNU_EH_FRAME);
  if (p->filesz > p->memsz)
    return;

  info.ehFrameAddr = getAddress<uint8_t>(p->vaddr);
  info.ehFrameSize = p->memsz;

  // custom struct for eh_frame_hdr
  struct GnuExceptionInfo {
    uint8_t version;
    uint8_t encoding;
    uint8_t fdeCount;
    uint8_t encodingTable;
    uint8_t first;
  };

  auto *exinfo = getOffset<GnuExceptionInfo>(p->offset);

  if (exinfo->version != 1)
    return;

  uint8_t *data_buffer = nullptr;
  uint8_t *current = &exinfo->first;

  if (exinfo->encoding == 0x03) // relative to base address
  {
    auto offset = *reinterpret_cast<uint32_t *>(current);
    current += 4;

    data_buffer = (uint8_t *)&info.base[offset];
  } else if (exinfo->encoding == 0x1B) // relative to eh_frame
  {
    auto offset = *reinterpret_cast<int32_t *>(current);
    current += 4;
    data_buffer = &current[offset];
  } else {
    return;
  }

  if (!data_buffer) {
    return;
  }

  uint8_t *data_buffer_end = data_buffer;
  while (true) {
    size_t size = *reinterpret_cast<int32_t *>(data_buffer_end);
    if (size == 0) {
      data_buffer_end = &data_buffer_end[4];
      break;
    }
    if (size == -1) {
      size = 12 + *reinterpret_cast<size_t *>(&data_buffer_end[4]);
    } else {
      size = 4 + size;
    }
    data_buffer_end = &data_buffer_end[size];
  }

  size_t fde_count;
  if (exinfo->fdeCount == 0x03) // absolute
  {
    fde_count = *reinterpret_cast<uint32_t *>(current);
    current += 4;
  } else {
    return;
  }

  if (exinfo->encodingTable != 0x3B) // relative to eh_frame
  {
    return;
  }

  info.ehFrameheaderAddr = data_buffer;
  info.ehFrameheaderSize = data_buffer_end - data_buffer;
}

void smodule::logDbgInfo() {
  for (uint16_t i = 0; i < elf->phnum; i++) {
    auto s = &segments[i];
    switch (s->type) {
    case PT_SCE_COMMENT: {
      // this is similar to the windows pdb path
      auto *comment = getOffset<SCEComment>(s->offset);

      std::string name;
      name.resize(comment->pathLength);
      memcpy(name.data(), getOffset<void>(s->offset + sizeof(SCEComment)),
             comment->pathLength);

      LOG_INFO("Starting: {}", name);
      break;
    }
#if 0
			case PT_SCE_LIBVERSION:
			{
				uint8_t* sec = getOffset<uint8_t>(s->offset);

				// count entries
				int32_t index = 0;
				while (index <= s->filesz) {

					int8_t cb = sec[index];

					// skip control byte
					index++;

					for (int i = index; i < (index + cb); i++)
					{
						if (sec[i] == 0x3A) {

							size_t length = i - index;

							std::string name;
							name.resize(length);
							memcpy(name.data(), &sec[index], length);

							uint32_t version = *(uint32_t*)& sec[i + 1];
							uint8_t* vptr = (uint8_t*)& version;

							std::printf("lib <%s>, version %x.%x.%x.%x\n", name.c_str(), vptr[0], vptr[1], vptr[2], vptr[3]);
							break;
						}
					}

					// skip forward
					index += cb;
				}
				break;
			}
#endif
    }
  }
}
} // namespace krnl