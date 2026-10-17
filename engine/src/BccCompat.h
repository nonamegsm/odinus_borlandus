#ifndef ODIN_BCC_COMPAT_H
#define ODIN_BCC_COMPAT_H
#include <stdint.h>
#include <stddef.h>
#if defined(__BORLANDC__)
#undef UINT64_C
#undef INT64_C
#define UINT64_C(value) value##ULL
#define INT64_C(value) value##LL
#endif
#include <algorithm>
namespace bcc {
template<class T, size_t N> class Array {
    T values_[N];
public:
    Array(){fill(T());}
    T* data(){return values_;} const T* data()const{return values_;}
    size_t size()const{return N;}
    T* begin(){return values_;} const T* begin()const{return values_;}
    T* end(){return values_+N;} const T* end()const{return values_+N;}
    T& operator[](size_t i){return values_[i];} const T& operator[](size_t i)const{return values_[i];}
    void fill(const T& value){std::fill(begin(),end(),value);}
    bool operator==(const Array& rhs)const{return std::equal(begin(),end(),rhs.begin());}
    bool operator!=(const Array& rhs)const{return !(*this==rhs);}
};
// Non-owning synchronous callbacks. The context must outlive the engine call.
template<class A> class Callback1 {
public:
    typedef void (*Function)(void*,A);
    Callback1(Function f=0,void* c=0):function_(f),context_(c){}
    operator bool()const{return function_!=0;}
    void operator()(A a)const{if(function_)function_(context_,a);}
private: Function function_;void* context_;
};
template<class A,class B> class Callback2 {
public:
    typedef void (*Function)(void*,A,B);
    Callback2(Function f=0,void* c=0):function_(f),context_(c){}
    operator bool()const{return function_!=0;}
    void operator()(A a,B b)const{if(function_)function_(context_,a,b);}
private: Function function_;void* context_;
};
}
#define BCC_JOIN2(a,b) a##b
#define BCC_JOIN(a,b) BCC_JOIN2(a,b)
#define BCC_STATIC_ASSERT(expr) typedef char BCC_JOIN(bcc_check_,__LINE__)[(expr)?1:-1]
#endif
