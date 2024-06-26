class MyClass
{
  int* m_p;

  void Foo() {
    int localVar;
    m_p = &localVar;
  }  // expected-note {{pointee 'localVar' left the scope here}}
  // expected-warning@-1 {{returning a dangling pointer as output value '(*this).m_p'}}

  void Foo2()
  {
    int a[3];
    m_p = &a[0];
  }  // expected-note {{pointee 'a' left the scope here}}
    // expected-warning@-1 {{returning a dangling pointer as output value '(*this).m_p'}}
};

void Get(float **x)
{
  float f;
  *x = &f;
}  // expected-warning {{returning a dangling pointer as output value '*x'}}
// expected-note@-1 {{pointee 'f' left the scope here}}

struct CVariable {
  char  name[64];
};

void test(int n, char *tokens[])
{
  for (int i=0;i<n;i++)
  {
    CVariable  var;
    tokens[i]  =  var.name;
  }  // expected-note {{pointee 'var' left the scope here}}
}  // expected-warning {{returning a dangling pointer as output value '*tokens'}}

struct Dummy {
    int* p;
};

void foo(Dummy* d)
{
    int x;
    d->p = &x;
}  // expected-note {{pointee 'x' left the scope here}}
  // expected-warning@-1 {{returning a dangling pointer as output value '(*d).p'}}

void foo2(Dummy*&& d)
{
    int x;
    d->p = &x;
}

float *F()
{
  float f = 1.0;
  return &f;
  // expected-warning@-1 {{returning a dangling pointer}}
  // expected-note@-2 {{pointee 'f' left the scope here}}
}

void memcpy(void *dst, const void *src, int n);

int *Foo(bool err)
{
  int A[10];
  // ...
  if (err)
    return 0;

  int *B = new int[10];
  memcpy(B, A, sizeof(A));

  return A;
  // expected-warning@-1 {{returning a dangling pointer}}
  // expected-note@-2 {{pointee 'A' left the scope here}}
}