// Training-only adapter for LightGBM 4.6.0's native C API. Evaluation and the
// application use only the numerical trees and do not load this DLL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

template<class Function>
Function entry(HMODULE dll, const char* name)
{
    auto address = GetProcAddress(dll,name);
    if (!address) throw std::runtime_error(std::string("Missing LightGBM entry point: ")+name);
    Function function;
    static_assert(sizeof(address) == sizeof(function));
    std::memcpy(&function,&address,sizeof(function));
    return function;
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc != 5) throw std::invalid_argument("Use fit.dll features.bin iterations output.txt");
        const auto dll = LoadLibraryExW(std::filesystem::absolute(argv[1]).c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) throw std::runtime_error("Could not load training DLL: "+std::to_string(GetLastError()));
        const auto lastError = entry<const char*(*)()>(dll,"LGBM_GetLastError");
        const auto check = [&](int status) { if (status) throw std::runtime_error(lastError()); };
        using Handle = void*;
        const auto createData = entry<int(*)(const void*,int,int32_t,int32_t,int,const char*,Handle,Handle*)>(dll,"LGBM_DatasetCreateFromMat");
        const auto setField = entry<int(*)(Handle,const char*,const void*,int,int)>(dll,"LGBM_DatasetSetField");
        const auto createBoost = entry<int(*)(Handle,const char*,Handle*)>(dll,"LGBM_BoosterCreate");
        const auto iterate = entry<int(*)(Handle,int*)>(dll,"LGBM_BoosterUpdateOneIter");
        const auto save = entry<int(*)(Handle,int,int,int,const char*)>(dll,"LGBM_BoosterSaveModel");
        const auto predict = entry<int(*)(Handle,const void*,int,int32_t,int32_t,int,int,int,int,const char*,int64_t*,double*)>(dll,"LGBM_BoosterPredictForMat");
        const auto freeBoost = entry<int(*)(Handle)>(dll,"LGBM_BoosterFree");
        const auto freeData = entry<int(*)(Handle)>(dll,"LGBM_DatasetFree");
        std::ifstream input(std::filesystem::path(argv[2]),std::ios::binary);
        input.exceptions(std::ios::badbit | std::ios::failbit);
        int32_t rows, cols;
        input.read(reinterpret_cast<char*>(&rows),sizeof(rows));
        input.read(reinterpret_cast<char*>(&cols),sizeof(cols));
        if (rows < 1 || cols < 1 || static_cast<int64_t>(rows)*cols > 100000000) throw std::invalid_argument("Training matrix size");
        std::vector<float> labels(rows), data(static_cast<size_t>(rows)*cols);
        for (int i = 0; i < rows; ++i) {
            input.read(reinterpret_cast<char*>(&labels[i]),sizeof(float));
            input.read(reinterpret_cast<char*>(&data[static_cast<size_t>(i)*cols]),cols*sizeof(float));
        }
        const int rounds = std::stoi(argv[3]);
        if (rounds < 1 || rounds > 4096) throw std::invalid_argument("Training rounds");
        const auto target = std::filesystem::path(argv[4]);
        if (std::filesystem::exists(target)) throw std::invalid_argument("Choose a new output");
        // Deterministic bins, leaf growth and column subsampling from fit rows only.
        const char* params = "objective=binary learning_rate=0.06 num_leaves=31 min_data_in_leaf=35 "
            "max_bin=63 feature_fraction=0.65 lambda_l2=2 num_threads=3 deterministic=true force_col_wise=true "
            "seed=1729 verbosity=-1";
        Handle dataset = nullptr, booster = nullptr;
        check(createData(data.data(),0,rows,cols,1,params,nullptr,&dataset));
        check(setField(dataset,"label",labels.data(),rows,0));
        check(createBoost(dataset,params,&booster));
        for (int i = 0; i < rounds; ++i) {
            int finished = 0;
            check(iterate(booster,&finished));
            if (finished) break;
        }
        check(save(booster,0,-1,0,target.string().c_str()));
        int64_t predicted = 0;
        std::vector<double> scores(rows);
        check(predict(booster,data.data(),0,rows,cols,1,0,0,-1,"num_threads=3",&predicted,scores.data()));
        if (predicted != rows) throw std::runtime_error("Native prediction count mismatch");
        std::ofstream reference(target.string()+".probabilities",std::ios::binary);
        reference.exceptions(std::ios::badbit|std::ios::failbit);
        reference.write(reinterpret_cast<const char*>(scores.data()),rows*sizeof(double));
        reference.close();
        check(freeBoost(booster));
        check(freeData(dataset));
        // OpenMP workers stay alive until process exit and may still execute
        // code from the DLL after the booster and training data are released.
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
