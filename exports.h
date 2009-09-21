// Naveen v1. #define EXPORT __declspec(dllexport)
#define EXPORT extern "C" __declspec(dllexport)
#define BIF(fun) void fun(ExprTokenType &aResultToken, ExprTokenType *aParam[], int aParamCount)

EXPORT unsigned int addFile(char *fileName, bool aAllowDuplicateInclude, int aIgnoreLoadFailure);
EXPORT int ahkdll(char *fileName, char *argv, char *args);
void BIF_Import(ExprTokenType &aResultToken, ExprTokenType *aParam[], int aParamCount);
void BIF_Getvar(ExprTokenType &aResultToken, ExprTokenType *aParam[], int aParamCount);
void BIF_sendahk(ExprTokenType &aResultToken, ExprTokenType *aParam[], int aParamCount); // Added in N11
BIF(BIF_Static) ;
BIF(BIF_Alias) ;
BIF(BIF_CacheEnable) ;
BIF(BIF_GetTokenValue) ;


EXPORT int ahkLabel(char *aLabelName);
EXPORT int ahkFunction(char *func, char *param1, char *param2, char *param3, char *param4);
bool callFunc(WPARAM awParam, LPARAM alParam);
// do not export callFunc, it must be called within script thread
void BIF_Import(ExprTokenType &aResultToken, ExprTokenType *aParam[], int aParamCount);
void BIF_FindFunc(ExprTokenType &aResultToken, ExprTokenType *aParam[], int aParamCount);
