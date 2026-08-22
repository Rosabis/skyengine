/* TinySoundFont implementation 编译单元。头部 tsf.h 是单头库:
 * 在恰好一个 .c 里 #define TSF_IMPLEMENTATION 从而实例化实现。
 * 注意不要定义 TSF_STATIC(那会让 API 变 static,其它翻译单元无法链接)。 */
#define TSF_IMPLEMENTATION
#include "tsf.h"