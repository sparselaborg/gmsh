// #ifndef MSQ_VERSION
// 
// /* Mesquite Version */
// #undef MSQ_VERSION
// 
// /* Mesquite Major Version */
// #undef MSQ_VERSION_MAJOR
// 
// /* Mesquite Minor Version */
// #undef MSQ_VERSION_MINOR
// 
// /* Mesquite Patch Level */
// #undef MSQ_VERSION_PATCH
// 
// /* Mesquite Version String */
// #undef MSQ_VERSION_STRING
// 
// /* Mesquite namespace */
// #undef MESQUITE_NS
// 
// /* Mesquite namespace alias */
// #undef MESQUITE_NS_ALIAS
// 
// #ifdef MESQUITE_NS_ALIAS
// namespace MESQUITE_NS {}
// namespace Mesquite = MESQUITE_NS;
// #endif
// 
// #endif

#ifndef MSQ_VERSION

/* Mesquite Version */
#define MSQ_VERSION 2.99

/* Mesquite Major Version */
#define MSQ_VERSION_MAJOR 2

/* Mesquite Minor Version */
#define MSQ_VERSION_MINOR 99

/* Mesquite Patch Level */
#define MSQ_VERSION_PATCH 0

/* Mesquite Version String */
#define MSQ_VERSION_STRING "Mesquite 2.99"

/* Mesquite namespace */
#define MESQUITE_NS Mesquite2

/* Mesquite namespace alias */
#define MESQUITE_NS_ALIAS 1

#ifdef MESQUITE_NS_ALIAS
namespace MESQUITE_NS {}
namespace Mesquite = MESQUITE_NS;
#endif

#endif