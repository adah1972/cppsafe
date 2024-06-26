// ARGS: --Wlifetime-output

bool foo1(bool x, int*& out)  // expected-note {{it was never initialized here}}
{
    if (x) {
        return false;  // expected-warning {{returning a dangling pointer as output value '*out'}}
    }

    out = new int{};
    return true;
}

bool foo2(bool x, int y, int*& out)
{
    out = &y;

    if (x) {
        return false;  // expected-warning {{returning a dangling pointer as output value '*out'}}
        // expected-note@-1 {{pointee 'y' left the scope here}}
    }

    out = new int{};
    return true;
}