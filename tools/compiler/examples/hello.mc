/* TinyOS MiniC 示例：CC.TNCR 能编译的程序
 * 编译： cc /home/hello.mc -o /bin/hello.TNCR
 * 运行： run /bin/hello.TNCR
 */

int counter;
char greeting[] = "hello from MiniC";
int table[5] = { 10, 20, 30, 40, 50 };

int add(int a, int b) {
    return a + b;
}

int fact(int n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
}

int sum_to(int n) {
    int s;
    int i;
    s = 0;
    for (i = 1; i <= n; i = i + 1) {
        s = s + i;
    }
    return s;
}

void show(char *tag, int v) {
    char num[16];
    api->print(tag);
    api->itoa(v, num);
    api->println(num);
}

int main() {
    char *msg;
    int i;
    msg = greeting;
    api->println(msg);
    api->println("--- MiniC demo ---");

    show("add(3,4)   = ", add(3, 4));
    show("fact(5)    = ", fact(5));
    show("sum_to(10) = ", sum_to(10));

    api->println("table:");
    for (i = 0; i < 5; i = i + 1) {
        api->print("  [");
        show("] = ", table[i]);
    }

    counter = 0;
    while (counter < 3) {
        counter = counter + 1;
    }
    show("counter = ", counter);

    if (fact(5) == 120) api->println("fact check: ok");
    else                api->println("fact check: FAILED");

    return 0;
}
