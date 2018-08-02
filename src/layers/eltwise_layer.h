//Tencent is pleased to support the open source community by making FeatherCNN available.

//Copyright (C) 2018 THL A29 Limited, a Tencent company. All rights reserved.

//Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
//in compliance with the License. You may obtain a copy of the License at
//
//https://opensource.org/licenses/BSD-3-Clause
//
//Unless required by applicable law or agreed to in writing, software distributed
//under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
//CONDITIONS OF ANY KIND, either express or implied. See the License for the
//specific language governing permissions and limitations under the License.

#pragma once

#include "../feather_simple_generated.h"
#include "../layer.h"
#include "arm/generic_kernels.h"

namespace feather
{
class EltwiseLayer : public Layer
{
public:
    EltwiseLayer(const LayerParameter* layer_param, const RuntimeParameter<float>* rt_param)
        :Layer(layer_param, rt_param)
    {
        _fusible = true;
        fuse_relu = false;
    }

    int Forward()
    {
        if(fuse_relu)
            add_relu<true>(output, input_alpha, input_beta, data_len, num_threads);
        else
            add_relu<false>(output, input_alpha, input_beta, data_len, num_threads);

        Layer::Forward();
        return 0;
    }

    int Fuse(Layer *next_layer)
    {
        if(next_layer->type().compare("ReLU") == 0)
        {
            printf("[FUSE] Eltwise with ReLU, %s, %s\n", this->name().c_str(), next_layer->name().c_str());
            fuse_relu = true;
            return 1;
        }
        else
            return 0;
    }

    int GenerateTopBlobs()
    {
        assert(_bottom.size() == 2);
        assert(_bottom_blobs.size() == 2);
        assert(_bottom_blobs[_bottom[0]]->data_size() == _bottom_blobs[_bottom[1]]->data_size());
        Blob<float>* p_blob = new Blob<float>();
        p_blob->CopyShape(_bottom_blobs[_bottom[0]]);
        //p_blob->Alloc();  //no need malloc, use net global input/output memory
        _top_blobs[_top[0]] = p_blob;
        return 0;
    }

    int Init(float *ginput, float *goutput)
    {
        (void)ginput;
        (void)goutput;
        input_alpha = _bottom_blobs[_bottom[0]]->data();
        input_beta  = _bottom_blobs[_bottom[1]]->data();
        output      = _top_blobs[_top[0]]->data();
        data_len    = _top_blobs[_top[0]]->data_size();
        return 0;
    }
private:
    float* input_alpha;
    float* input_beta;
    size_t data_len;
    bool fuse_relu;
};
};
