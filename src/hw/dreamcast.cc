#include "core/core.h"
#include "hw/dreamcast.h"
#include "jit/backend/interpreter/interpreter_backend.h"
#include "jit/backend/x64/x64_backend.h"
#include "jit/frontend/sh4/sh4_frontend.h"

using namespace dreavm;
using namespace dreavm::hw;
using namespace dreavm::hw::aica;
using namespace dreavm::hw::gdrom;
using namespace dreavm::hw::holly;
using namespace dreavm::hw::maple;
using namespace dreavm::hw::sh4;
using namespace dreavm::jit;
using namespace dreavm::jit::backend::interpreter;
using namespace dreavm::jit::backend::x64;
using namespace dreavm::jit::frontend::sh4;
using namespace dreavm::renderer;
using namespace dreavm::sys;
using namespace dreavm::trace;

Dreamcast::Dreamcast()
    :  // allocate registers and initialize references
      holly_regs_(new Register[HOLLY_REG_SIZE >> 2]),
#define HOLLY_REG(offset, name, flags, default, type) \
  name{reinterpret_cast<type &>(holly_regs_[name##_OFFSET].value)},
#include "hw/holly/holly_regs.inc"
#undef HOLLY_REG
      pvr_regs_(new Register[PVR_REG_SIZE >> 2]),
#define PVR_REG(offset, name, flags, default, type) \
  name{reinterpret_cast<type &>(pvr_regs_[name##_OFFSET].value)},
#include "hw/holly/pvr2_regs.inc"
#undef PVR_REG
      rb_(nullptr),
      trace_writer_(nullptr) {
// initialize register values
#define HOLLY_REG(addr, name, flags, default, type) \
  holly_regs_[name##_OFFSET] = {flags, default};
#include "hw/holly/holly_regs.inc"
#undef HOLLY_REG

#define PVR_REG(addr, name, flags, default, type) \
  pvr_regs_[name##_OFFSET] = {flags, default};
#include "hw/holly/pvr2_regs.inc"
#undef PVR_REG

  aica_regs_ = new uint8_t[AICA_REG_SIZE]();
  bios_ = new uint8_t[BIOS_SIZE]();
  expdev_mem_ = new uint8_t[EXPDEV_SIZE]();
  flash_ = new uint8_t[FLASH_SIZE]();
  modem_mem_ = new uint8_t[MODEM_REG_SIZE]();
  palette_ram_ = new uint8_t[PVR_PALETTE_SIZE]();
  ram_ = new uint8_t[MAIN_RAM_SIZE]();
  unassigned_ = new uint8_t[UNASSIGNED_SIZE]();
  video_ram_ = new uint8_t[PVR_VRAM32_SIZE]();
  wave_ram_ = new uint8_t[WAVE_RAM_SIZE]();

  scheduler_ = new Scheduler();
  memory_ = new Memory();
  rt_frontend_ = new SH4Frontend(*memory_);
  rt_backend_ = new X64Backend(*memory_);
  runtime_ = new Runtime(*memory_, *rt_frontend_, *rt_backend_);
  aica_ = new AICA(this);
  gdrom_ = new GDROM(this);
  holly_ = new Holly(this);
  maple_ = new Maple(this);
  pvr_ = new PVR2(this);
  sh4_ = new SH4(*memory_, *runtime_);
  ta_ = new TileAccelerator(this);
  texcache_ = new TextureCache(this);
  tile_renderer_ = new TileRenderer(*texcache_);
}

Dreamcast::~Dreamcast() {
  delete[] holly_regs_;
  delete[] pvr_regs_;

  delete bios_;
  delete flash_;
  delete ram_;
  delete unassigned_;
  delete modem_mem_;
  delete aica_regs_;
  delete wave_ram_;
  delete expdev_mem_;
  delete video_ram_;
  delete palette_ram_;

  delete scheduler_;
  delete memory_;
  delete rt_frontend_;
  delete rt_backend_;
  delete runtime_;
  delete aica_;
  delete gdrom_;
  delete holly_;
  delete maple_;
  delete pvr_;
  delete sh4_;
  delete ta_;
  delete texcache_;
  delete tile_renderer_;
}

bool Dreamcast::Init() {
  MapMemory();

  if (!aica_->Init()) {
    return false;
  }

  if (!gdrom_->Init()) {
    return false;
  }

  if (!holly_->Init()) {
    return false;
  }

  if (!maple_->Init()) {
    return false;
  }

  if (!pvr_->Init()) {
    return false;
  }

  if (!sh4_->Init()) {
    return false;
  }

  if (!ta_->Init()) {
    return false;
  }

  if (!texcache_->Init()) {
    return false;
  }

  scheduler_->AddDevice(aica_);
  scheduler_->AddDevice(sh4_);

  return true;
}

void Dreamcast::MapMemory() {
  using namespace std::placeholders;

  // main ram
  memory_->Mount(BIOS_START, BIOS_END, MIRROR_MASK, bios_);
  memory_->Mount(FLASH_START, FLASH_END, MIRROR_MASK, flash_);
  memory_->Mount(MAIN_RAM_START, MAIN_RAM_END, MAIN_RAM_MIRROR_MASK, ram_);
  memory_->Mount(UNASSIGNED_START, UNASSIGNED_END, MIRROR_MASK, unassigned_);

  // aica
  memory_->Handle(AICA_REG_START, AICA_REG_END, MIRROR_MASK, aica(),
                  nullptr,               //
                  nullptr,               //
                  &AICA::ReadRegister,   //
                  nullptr,               //
                  nullptr,               //
                  nullptr,               //
                  &AICA::WriteRegister,  //
                  nullptr);
  memory_->Handle(WAVE_RAM_START, WAVE_RAM_END, MIRROR_MASK, aica(),
                  nullptr,           //
                  nullptr,           //
                  &AICA::ReadWave,   //
                  nullptr,           //
                  nullptr,           //
                  nullptr,           //
                  &AICA::WriteWave,  //
                  nullptr);

  // holly
  memory_->Handle(HOLLY_REG_START, HOLLY_REG_END, MIRROR_MASK, holly(),
                  nullptr,                //
                  nullptr,                //
                  &Holly::ReadRegister,   //
                  nullptr,                //
                  nullptr,                //
                  nullptr,                //
                  &Holly::WriteRegister,  //
                  nullptr);
  memory_->Mount(MODEM_REG_START, MODEM_REG_END, MIRROR_MASK, modem_mem_);
  memory_->Mount(EXPDEV_START, EXPDEV_END, MIRROR_MASK, expdev_mem_);

  // gdrom
  memory_->Handle(GDROM_REG_START, GDROM_REG_END, MIRROR_MASK, gdrom(),
                  &GDROM::ReadRegister<uint8_t>,    //
                  &GDROM::ReadRegister<uint16_t>,   //
                  &GDROM::ReadRegister<uint32_t>,   //
                  nullptr,                          //
                  &GDROM::WriteRegister<uint8_t>,   //
                  &GDROM::WriteRegister<uint16_t>,  //
                  &GDROM::WriteRegister<uint32_t>,  //
                  nullptr);

  // maple
  memory_->Handle(MAPLE_REG_START, MAPLE_REG_END, MIRROR_MASK, maple(),
                  nullptr,                //
                  nullptr,                //
                  &Maple::ReadRegister,   //
                  nullptr,                //
                  nullptr,                //
                  nullptr,                //
                  &Maple::WriteRegister,  //
                  nullptr);

  // pvr2
  memory_->Handle(PVR_VRAM32_START, PVR_VRAM32_END, MIRROR_MASK, pvr(),
                  &PVR2::ReadVRam<uint8_t>,    //
                  &PVR2::ReadVRam<uint16_t>,   //
                  &PVR2::ReadVRam<uint32_t>,   //
                  nullptr,                     //
                  nullptr,                     //
                  &PVR2::WriteVRam<uint16_t>,  //
                  &PVR2::WriteVRam<uint32_t>,  //
                  nullptr);
  memory_->Handle(PVR_VRAM64_START, PVR_VRAM64_END, MIRROR_MASK, pvr(),
                  &PVR2::ReadVRamInterleaved<uint8_t>,    //
                  &PVR2::ReadVRamInterleaved<uint16_t>,   //
                  &PVR2::ReadVRamInterleaved<uint32_t>,   //
                  nullptr,                                //
                  nullptr,                                //
                  &PVR2::WriteVRamInterleaved<uint16_t>,  //
                  &PVR2::WriteVRamInterleaved<uint32_t>,  //
                  nullptr);
  memory_->Handle(PVR_REG_START, PVR_REG_END, MIRROR_MASK, pvr(),
                  nullptr,               //
                  nullptr,               //
                  &PVR2::ReadRegister,   //
                  nullptr,               //
                  nullptr,               //
                  nullptr,               //
                  &PVR2::WriteRegister,  //
                  nullptr);
  memory_->Handle(PVR_PALETTE_START, PVR_PALETTE_END, MIRROR_MASK, pvr(),
                  nullptr,              //
                  nullptr,              //
                  nullptr,              //
                  nullptr,              //
                  nullptr,              //
                  nullptr,              //
                  &PVR2::WritePalette,  //
                  nullptr);

  // ta
  // TODO handle YUV transfers from 0x10800000 - 0x10ffffe0
  memory_->Handle(TA_CMD_START, TA_CMD_END, 0x0, ta(),
                  nullptr,                         //
                  nullptr,                         //
                  nullptr,                         //
                  nullptr,                         //
                  nullptr,                         //
                  nullptr,                         //
                  &TileAccelerator::WriteCommand,  //
                  nullptr);
  memory_->Handle(TA_TEXTURE_START, TA_TEXTURE_END, 0x0, ta(),
                  nullptr,                         //
                  nullptr,                         //
                  nullptr,                         //
                  nullptr,                         //
                  nullptr,                         //
                  nullptr,                         //
                  &TileAccelerator::WriteTexture,  //
                  nullptr);

  // cpu
  memory_->Handle(SH4_REG_START, SH4_REG_END, MIRROR_MASK, sh4(),
                  &SH4::ReadRegister<uint8_t>,
                  &SH4::ReadRegister<uint16_t>,   //
                  &SH4::ReadRegister<uint32_t>,   //
                  nullptr,                        //
                  &SH4::WriteRegister<uint8_t>,   //
                  &SH4::WriteRegister<uint16_t>,  //
                  &SH4::WriteRegister<uint32_t>,  //
                  nullptr);
  memory_->Handle(SH4_CACHE_START, SH4_CACHE_END, 0x0, sh4(),
                  &SH4::ReadCache<uint8_t>,    //
                  &SH4::ReadCache<uint16_t>,   //
                  &SH4::ReadCache<uint32_t>,   //
                  &SH4::ReadCache<uint64_t>,   //
                  &SH4::WriteCache<uint8_t>,   //
                  &SH4::WriteCache<uint16_t>,  //
                  &SH4::WriteCache<uint32_t>,  //
                  &SH4::WriteCache<uint64_t>);
  memory_->Handle(SH4_SQ_START, SH4_SQ_END, 0x0, sh4(),
                  &SH4::ReadSQ<uint8_t>,    //
                  &SH4::ReadSQ<uint16_t>,   //
                  &SH4::ReadSQ<uint32_t>,   //
                  nullptr,                  //
                  &SH4::WriteSQ<uint8_t>,   //
                  &SH4::WriteSQ<uint16_t>,  //
                  &SH4::WriteSQ<uint32_t>,  //
                  nullptr);
}