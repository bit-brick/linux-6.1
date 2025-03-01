// SPDX-License-Identifier: GPL-2.0
#ifndef __CSC_MATRIX_h__
#define __CSC_MATRIX_h__
#include "v2d_drv.h"
int cscmatrix[V2D_CSC_MODE_BUTT][3][4] = {
//RGB2BT601Wide
    {{  306,  601, 117,   0 },
     { -173, -339, 512, 128 },
     {  512, -429, -83, 128 }},

//BT601Wide2RGB
    {{ 1024,    0, 1436, -179 },
     { 1024, -352, -731,  135 },
     { 1024, 1815,    0, -227 }},

//RGB2BT601Narrow
    {{  263,  516, 100,  16 },
     { -152, -298, 450, 128 },
     {  450, -377, -73, 128 }},

//BT601Narrow2RGB
    {{ 1192,    0, 1634, -223 },
     { 1192, -401, -832,  136 },
     { 1192, 2066,    0, -277 }},

//RGB2BT709Wide
    {{  218,  732,   74,   0 },
     { -117, -395,  512, 128 },
     {  512, -465,  -47, 128 }},

//BT709Wide2RGB
    {{ 1024,    0, 1613, -202 },
     { 1024, -192, -479,   84 },
     { 1024, 1900,    0, -238 }},

//RGB2BT709Narrow
    {{  187,  629,  63, 16 },
     { -103, -347, 450, 128},
     {  450, -409, -41, 128}},

//BT709Narrow2RGB
    {{ 1192,    0, 1836, -248 },
     { 1192, -218, -546,   77 },
     { 1192, 2163,    0, -289 }},

//BT601Wide2BT709Wide
    {{ 1024, -121, -218,  42 },
     {    0, 1043,  117, -17 },
     {    0,   77, 1050, -13 }},

//BT601Wide2BT709Narrow
    {{ 879, -104, -187, 52 },
     {   0,  916,  103,  1 },
     {   0,   68,  922,  4 }},

//BT601Wide2BT601Narrow
    {{ 879,   0,   0,  16 },
     {   0, 900,   0,  16 },
     {   0,   0, 900,  16 }},

//BT601Narrow2BT709Wide
    {{ 1192, -138, -248,   30 },
     {   0,  1187,  134,  -37 },
     {   0,    88, 1195,  -32 }},

//BT601Narrow2BT709Narrow
    {{ 1024, -118, -213,  41 },
     {    0, 1043,  117, -17 },
     {    0,   77, 1050, -13 }},

//BT601Narrow2BT601Wide
    {{ 1192,    0,    0, -19 },
     {    0, 1166,    0, -18 },
     {    0,    0, 1166, -18 }},

//BT709Wide2BT601Wide
    { { 1024,  104,  201,  -38 },
        {   0, 1014, -113,   15 },
        {   0,  -74, 1007,   11 } },

//BT709Wide2BT601Narrow
    {{ 879,  89,  172, -17 },
     {   0, 890, -100,  29 },
     {   0, -65,  885,  26 }},

//BT709Wide2BT709Narrow
    {{ 879,   0,   0,  16 },
     {   0, 900,   0,  16 },
     {   0,   0, 900,  16 }},

//BT709Narrow2BT601Wide
    {{ 1192,  118,  229,  -62 },
     {    0, 1154, -129,    0 },
     {    0,  -85, 1146,   -5 }},

//BT709Narrow2BT601Narrow
    {{ 1024,  102,  196,  -37 },
     {    0, 1014, -113,   15 },
     {    0,  -74, 1007,   11 }},

//BT709Narrow2BT709Wide
    {{ 1192,    0,    0, -19 },
     {    0, 1166,    0, -18 },
     {    0,    0, 1166, -18 }},

    //RGB2Grey
    {{  218,  732,  74,   0  },
     { -117, -395, 512,  128 },
     {  512, -465, -47,  128 }},

    //RGB2RGB
    {{ 1024,    0,    0, 0 },
     {    0, 1024,    0, 0 },
     {    0,    0, 1024, 0 }}
};
#endif
