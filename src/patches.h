#pragma once

#include "app.h"
#include <string_view>

void mcla_ApplyPatches(mcla::App::FunctionDispatcher* dispatcher);

bool BisectGroupEnabled(std::string_view group);



extern "C" void sub_82554080_stub(mcla::PPCContext& ctx, uint8_t* base);
extern "C" void hk_press_start_shim(mcla::PPCContext& ctx, uint8_t* base);