// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <sstream>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_STEPS = 10000;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack(): top(nullptr), count(0)
    { // initialize the stack
    }
    ~Stack(){ while (!isEmpty()){pop();}}

    void push(const T &val)
    {
        // pushes the value on the stack if max limit is not reached yet.
        if (count >= MAX_STACK_DEPTH) return;
        Node *node = new Node{val, top};
        top = node;
        count++;
    }
    T pop()
    {
        // pop the top value on the stack
        if (isEmpty()) return T{};
        Node *old = top;
        T val = old->data;
        top = top->next;
        delete old;
        count--;
        return val;
    }
    T &peek()
    {
        // returns the top value on the stack
        static T dummy{};
        if (isEmpty()) return dummy;
        return top->data;
    }
    bool isEmpty(){return top == nullptr;}

    bool isFull() const{ return count >= MAX_STACK_DEPTH; }

    int32_t depth(){return count;}

    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        int32_t idx = 0;
        Node *curr = top;
        while (curr != nullptr && idx < maxLen){
            out[idx++] = curr->data;
            curr = curr->next;
        }
        return idx;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline(): head(nullptr), tail(nullptr), stepCount(0) {}

    ~Timeline();

    void record(Snapshot *s)
    {
        // add record in the timeline
        TimelineNode *node = new TimelineNode{s, nullptr, tail};
        if (tail != nullptr)
        {
            tail->next = node;
        }
        else
        {
            head = node;
        }
        tail = node;
        stepCount++;
    }

    TimelineNode *begin(){return head;}

    int32_t getStepCount(){return stepCount;}

};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

Timeline::~Timeline()
{
    TimelineNode *curr = head;
    while (curr != nullptr)
    {
        TimelineNode *nxt = curr->next;
        delete curr->data;
        delete curr;
        curr = nxt;
    }
}

struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK

static bool isBlankSp(char c){ return c == ' ' or c == '\t' or c == '\r' or c == '\n'; }

static bool lineHasChar(const string& line){
    for(auto c : line){ if(!isBlankSp(c)){ return true; } }
    return false;
}

bool readSourceLine(ifstream &in, string &out)
{
    // reads the next nonblank line
    while(getline(in, out)){
        if(!out.empty() and out.back() == '\r'){out.pop_back();}
        if(lineHasChar(out)){ return true; }
    }
    return false;
}

static string nthWord(const string &line, int n){
    auto itrtr = line.begin();
    for(int i = 0; i <= n; i++){
        while(itrtr != line.end() and isBlankSp(*itrtr)){ itrtr++; }
        auto st = itrtr;
        while(itrtr != line.end() and !isBlankSp(*itrtr)){ itrtr++; }
        if(i == n){ return string(st, itrtr); }
    }
    return "";
}

string firstWord(const string &line)
{
    // returns first word from the input string
    return nthWord(line,0);
}
string secondWord(const string &line)
{
    // returns the second word
    return nthWord(line,1);
}
bool validateProgram(const char *sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed -
    bool inFunc = false;
    int lineNum = 0;
    string line;

    ifstream fin(sourcePath);
    if(!fin){
        cerr << "Validation Error: Cannot Open " << sourcePath << "\n";
        return false;
    }

    while(readSourceLine(fin, line)){
        lineNum++;
        string word = firstWord(line);

        if(word == "func"){
            if(inFunc){
                cerr << "Error: Nested Function At Line " << lineNum << "\n";
                return false;
            }
            inFunc = true;
        }
        else if(word == "func_end"){
            if(!inFunc){
                cerr << "Error: No Function Open To Use func_end At Line " << lineNum << "\n";
                return false;
            }
            inFunc = false;
        }
    }
    if(inFunc){
        cerr << "Validatioin Error: Missing func_end At The End Of The File...\n";
        return false;
    }
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t st = ftell(f);
    int32_t size = (int32_t)text.size();

    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    if(size > 0){ fwrite(text.data(), 1, size, f); }
    return st;
}

int64_t readResolveRecord(FILE *f, string &outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    int64_t offsetField;
    int32_t size;

    if(fread(&offsetField, sizeof(int64_t), 1, f) != 1){ return -1; }
    if(fread(&size, sizeof(int32_t), 1, f) != 1){ return -1; }
    if(size < 0){ return -1; }

    outText.assign(size, '\0');
    if(size > 0 and fread(&outText[0], 1, size, f) != (size_t)size){ return -1; }
    return offsetField;
}

int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error

    ifstream fin(sourcePath);
    if(!fin){
        cerr << "Resolve Error: Cannot Open " << sourcePath << "\n";
        return -1;
    }

    FILE *out = fopen(resolveBinPath, "w+b");
    if(out == nullptr){
        cerr << "Resolve Error: Cannot Create " <<resolveBinPath << "\n";
        return -1;
    }

    string line;
    int lineNum = 0;
    while(readSourceLine(fin, line)){
        lineNum++;
        string word = firstWord(line);
        int64_t pos = ftell(out);

        if(word == "func"){
            string name = secondWord(line);
            if(name.empty()){
                cerr << "Resolve Error: Function Is Without A Name At Line " << lineNum << "\n";
                goto fail;
            }
            for(int32_t i = 0; i < funcCount; i++){
                if(funcArray[i].funcName == name){
                    cerr << "Resolve Error: Duplicate Function '" << name << "' at line " << lineNum << "\n";
                    goto fail;
                }
            }
            if(funcCount >= MAX_FUNCS){
                cerr << "Resolve Error: Too Many Functions (max " << MAX_FUNCS << " )\n";
                goto fail;
            }
            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;

            writeResolveRecord(out, pos, line);
        }
        else if(word == "call"){
            string target = secondWord(line);
            if(target.empty()){
                cerr << "Resolve Error: Call Without A Target At Line " << lineNum << "\n";
                goto fail;
            }
            if(patchCount >= MAX_PATCHES){
                cerr << "Resolve Error: Too Many Call Instruction (max " << MAX_PATCHES << ")\n";
                goto fail;
            }
            patches[patchCount].byteOffsetOfOffsetField = pos;
            patches[patchCount].targetFuncName = target;
            patchCount++;

            writeResolveRecord(out, -1, line);
        }
        else{ writeResolveRecord(out, pos, line); }
    }

    for(int32_t p = 0; p < patchCount; p++){
        int64_t target = -1;
        for(int32_t i = 0; i < funcCount; i++){
            if(funcArray[i].funcName == patches[p].targetFuncName){
                target = funcArray[i].byteOffsetInResolveBin;
                break;
            }
        }
        if(target < 0){
            cerr << "Resolve Error: Call To Undefined Function '" << patches[p].targetFuncName << "'\n";
            goto fail;
        }
        fseek(out, (long)patches[p].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&target, sizeof(int64_t), 1, out);
    }
    {
        int64_t mainOffset = -1;
        for(int32_t i = 0; i < funcCount; i++){
            if(funcArray[i].funcName == "main"){
                mainOffset = funcArray[i].byteOffsetInResolveBin;
                break;
            }
        }
        if(mainOffset < 0){
            cerr << "Resolve Error: No Main Function\n";
            goto fail;
        }
        fclose(out);
        return mainOffset;
    }
fail:
    fclose(out);
    remove(resolveBinPath);
    return -1;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    istringstream iss(line);
    string Line;
    int32_t n = 0;
    while(iss >> Line){
        if(n >= maxTokens){ return -1; }
        tokens[n].text = Line;
        tokens[n].type = (n == 0) ? KEYWORD : (n == 1) ? IDENTIFIER : PARAM;
        n++;
    }
    return n;
}

Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
    Snapshot *s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}

static bool isNumber(const string &s)
{
    size_t i = 0;
    if (s.size() > 1 && (s[0] == '-' || s[0] == '+'))
        i = 1;
    if (i >= s.size())
        return false;
    for (; i < s.size(); i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

static Variable *findVar(Frame &fr, const string &name)
{
    for (int32_t i = 0; i < fr.argc; i++)
        if (fr.argv[i].name == name)
            return &fr.argv[i];
    for (int32_t i = 0; i < fr.localCount; i++)
        if (fr.locals[i].name == name)
            return &fr.locals[i];
    return nullptr;
}

static bool getValue(Frame &fr, const string &text, int32_t &out)
{
    if (isNumber(text))
    {
        long long v = strtoll(text.c_str(), nullptr, 10);
        if (v < INT32_MIN || v > INT32_MAX)
            return false;
        out = (int32_t)v;
        return true;
    }
    Variable *v = findVar(fr, text);
    if (v == nullptr)
        return false;
    out = v->value;
    return true;
}

bool executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
     FILE *f = fopen(resolveBinPath, "rb");
    if (f == nullptr)
    {
        cerr << "Execution error: cannot open " << resolveBinPath << "\n";
        return false;
    }
    auto fail = [&](const string &msg)
    {
        cerr << "Execution error: " << msg << "\n";
        fclose(f);
        return false;
    };

    Stack<Frame> callStack;
    // argSrc[d][i] = name of the CALLER's variable that was passed as arg i
    // to the frame at depth index d ("" if a literal was passed)
    string argSrc[MAX_STACK_DEPTH][MAX_VARS_PER_FRAME];
    Token tokens[MAX_TOKENS];
    string text;
    int32_t steps = 0;

    // read main's FUNC header and push its frame
    fseek(f, (long)mainOffset, SEEK_SET);
    if (readResolveRecord(f, text) < 0)
        return fail("cannot read main's header");
    Frame mainFrame{};
    mainFrame.func_name = "main";
    mainFrame.returnLine = -1;
    callStack.push(mainFrame);

    while (true)
    {
        if (++steps > MAX_STEPS)
            return fail("step limit exceeded");

        int64_t off = readResolveRecord(f, text);
        if (off < 0)
            return fail("ran past the end of resolve.bin");

        if (firstWord(text).compare(0, 2, "//") == 0)
            continue;

        int32_t n = tokenizeLine(text, tokens, MAX_TOKENS);
        if (n <= 0)
            return fail("cannot tokenize line: " + text);

        const string kw = tokens[0].text;
        if (kw.compare(0, 2, "//") == 0)
            continue; // comment: not an instruction, no snapshot

        Frame &cur = callStack.peek();

        if (kw == "set")
        {
            if (n != 3)
                return fail("set needs a name and a value: " + text);
            int32_t v;
            if (!getValue(cur, tokens[2].text, v))
                return fail("bad value in: " + text);
            Variable *var = findVar(cur, tokens[1].text);
            if (var == nullptr)
            {
                if (cur.localCount >= MAX_VARS_PER_FRAME)
                    return fail("too many variables in " + cur.func_name);
                var = &cur.locals[cur.localCount++];
                var->name = tokens[1].text;
            }
            var->value = v;
        }
        else if (kw == "add" || kw == "sub" || kw == "mul" || kw == "div")
        {
            if (n != 3)
                return fail(kw + " needs two operands: " + text);
            Variable *dst = findVar(cur, tokens[1].text);
            if (dst == nullptr)
                return fail("undefined variable '" + tokens[1].text + "'");
            int32_t rhs;
            if (!getValue(cur, tokens[2].text, rhs))
                return fail("bad operand in: " + text);

            int64_t a = dst->value, r = 0;
            if (kw == "add") r = a + rhs;
            else if (kw == "sub") r = a - rhs;
            else if (kw == "mul") r = a * rhs;
            else
            {
                if (rhs == 0)
                    return fail("division by zero: " + text);
                r = a / rhs;
            }
            dst->value = (int32_t)r;
        }
        else if (kw == "call")
        {
            if (off < 0)
                return fail("unresolved call: " + text);
            if (callStack.isFull())
                return fail("stack overflow (max depth reached)");

            int64_t retPos = ftell(f); // record right after this call
            int32_t argCount = n - 2;

            // read the callee's FUNC header to get its parameter names
            fseek(f, (long)off, SEEK_SET);
            string hdr;
            if (readResolveRecord(f, hdr) < 0)
                return fail("cannot read callee header");
            Token ht[MAX_TOKENS];
            int32_t hn = tokenizeLine(hdr, ht, MAX_TOKENS);
            if (hn < 2 || ht[0].text != "func")
                return fail("call target is not a function: " + text);
            int32_t paramCount = hn - 2;
            if (paramCount != argCount)
                return fail("wrong number of arguments: " + text);

            Frame nf{};
            nf.func_name = ht[1].text;
            nf.argc = paramCount;
            nf.returnLine = (int32_t)retPos;
            int32_t d = callStack.depth(); // index the new frame will have
            for (int32_t i = 0; i < paramCount; i++)
            {
                const string &arg = tokens[2 + i].text;
                int32_t v;
                if (!getValue(cur, arg, v))
                    return fail("bad argument '" + arg + "' in: " + text);
                nf.argv[i].name = ht[2 + i].text;
                nf.argv[i].value = v;
                argSrc[d][i] = isNumber(arg) ? "" : arg;
            }
            callStack.push(nf);
            // file position is now just past the callee's header, so the
            // next loop iteration runs the callee's first line
        }
        else if (kw == "func_end")
        {
            Frame done = callStack.pop();
            int32_t d = callStack.depth(); // index the popped frame had

            if (callStack.isEmpty())
            { // main finished
                timeline.record(buildSnapshot(callStack));
                break;
            }
            // copy-out: write the params' final values back to the caller
            Frame &caller = callStack.peek();
            for (int32_t i = 0; i < done.argc; i++)
            {
                if (argSrc[d][i].empty())
                    continue;
                Variable *cv = findVar(caller, argSrc[d][i]);
                if (cv != nullptr)
                    cv->value = done.argv[i].value;
            }
            fseek(f, (long)done.returnLine, SEEK_SET); // resume after the call
        }
        else
        {
            return fail("unknown or misplaced instruction: " + text);
        }

        timeline.record(buildSnapshot(callStack)); // one snapshot per executed line
    }

    fclose(f);
    return true;
}

// PASS 0x3: SERIALIZE TIMELINE
static void writeInt32(FILE *f, int32_t v){ fwrite(&v, sizeof(int32_t), 1, f); }

static void writeString(FILE *f, const string &s)
{
    writeInt32(f, (int32_t)s.size());
    if (!s.empty()){ fwrite(s.data(), 1, s.size(), f); }
}

static void writeVariable(FILE *f, const Variable &v)
{
    writeString(f, v.name);
    writeInt32(f, v.value);
}

static void writeSnapshot(FILE *f, const Snapshot &s)
{
    writeInt32(f, s.stackDepth);
    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame &fr = s.callStack[i];
        writeString(f, fr.func_name);
        writeInt32(f, fr.argc);
        for (int32_t a = 0; a < fr.argc; a++)
            writeVariable(f, fr.argv[a]);
        writeInt32(f, fr.returnLine);
        writeInt32(f, fr.localCount);
        for (int32_t l = 0; l < fr.localCount; l++)
            writeVariable(f, fr.locals[l]);
    }
}


bool writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
    FILE *f = fopen(tdbgPath, "wb");
    if (f == nullptr)
    {
        cerr << "Serialize error: cannot create " << tdbgPath << "\n";
        return false;
    }

    int32_t steps = timeline.getStepCount();

    // 1. placeholder header (indexOffset unknown yet)
    TTDBHeader h;
    h.magic[0] = 'T'; h.magic[1] = 'T'; h.magic[2] = 'D'; h.magic[3] = 'B';
    h.version = 1;
    h.stepCount = steps;
    h.indexOffset = 0;
    writeHeader(f, h);

    // 2. snapshot stream, remembering where each one starts
    int64_t *index = new int64_t[steps > 0 ? steps : 1];
    int32_t i = 0;
    for (TimelineNode *node = timeline.begin(); node != nullptr && i < steps; node = node->next)
    {
        index[i++] = ftell(f);
        writeSnapshot(f, *node->data);
    }

    // 3. dense index trailer: one int64 per step
    h.indexOffset = ftell(f);
    fwrite(index, sizeof(int64_t), steps, f);
    delete[] index;

    // 4. go back and write the real header
    fseek(f, 0, SEEK_SET);
    writeHeader(f, h);

    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
    {
        cerr << "Serialize error: write failed\n";
        remove(tdbgPath);
    }
    return ok;
}
// main section
int32_t main()
{
    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset < 0)
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    Timeline timeline;
    if (!executeProgram("resolve.bin", mainOffset, timeline))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    if (!writeTdbg(timeline, "session.tdbg"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    cout << "session.tdbg written (" << timeline.getStepCount() << " steps)\n";
    return 0;
}