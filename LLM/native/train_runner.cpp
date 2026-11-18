// train_runner.cpp - Huáº¥n luyá»‡n & Há»ŒC LIÃŠN Tá»¤C Bit-LLM v16 "Bit-NNUE" (cháº¡y native, vd. Colab)
// BiÃªn dá»‹ch:  make train        (xem Makefile; cáº§n g++ cÃ³ OpenMP)
//
// CÃ¡ch dÃ¹ng:
//   Train má»›i:                 ./train_runner 100 --data dataset.txt
//   Há»c thÃªm dataset má»›i:      ./train_runner 40 --resume --data dataset_moi.txt
//
// Tuá»³ chá»n:
//   --data <file>        dataset "há»i | Ä‘Ã¡p" (hoáº·c phÃ¢n cÃ¡ch báº±ng TAB), máº·c Ä‘á»‹nh dataset.txt
//   --out <file>         tá»‡p mÃ´ hÃ¬nh .bin xuáº¥t ra (máº·c Ä‘á»‹nh bit_llm_weights.bin)
//   --resume             náº¡p --out rá»“i há»c tiáº¿p (tá»« vá»±ng/df/kho truy há»“i Ä‘Æ°á»£c káº¿ thá»«a)
//   --state <file>       trá»ng sá»‘ FLOAT (master) Ä‘á»ƒ há»c tiáº¿p khÃ´ng máº¥t pháº§n láº» lÆ°á»£ng tá»­ hoÃ¡ (máº·c Ä‘á»‹nh bit_llm_state.bin).
//                        Chá»‰ dÃ¹ng Ä‘Æ°á»£c khi khá»›p .bin (cÃ¹ng tá»« vá»±ng + cÃ¹ng sá»‘ epoch). KhÃ´ng lÆ°u Adam moment (khá»Ÿi Ä‘á»™ng láº¡i lÃ  bÃ¬nh thÆ°á»ng).
//   --no-state           khÃ´ng lÆ°u/náº¡p state float (tiáº¿t kiá»‡m ~40 MB Ä‘Ä©a)
//   --lr <x>             tá»‘c Ä‘á»™ há»c Ä‘á»‰nh, máº·c Ä‘á»‹nh 0.003 (lá»‹ch giáº£m tuyáº¿n tÃ­nh Ä‘áº¿n 5%)
//   --neg <k>            sá»‘ Ã¢m láº¥y máº«u má»—i bÆ°á»›c (1..64), máº·c Ä‘á»‹nh 24
//   --hn-every <k>       quÃ©t toÃ n tá»« vá»±ng Ä‘á»ƒ lÃ m má»›i hard-negative má»—i k bÆ°á»›c, máº·c Ä‘á»‹nh 16
//   --logq <0..2>        há»‡ sá»‘ hiá»‡u chá»‰nh logQ cho sampled-softmax (0 = táº¯t, 1 = Ä‘áº§y Ä‘á»§)
//   --holdout <pháº§n trÄƒm> cáº·p giá»¯-láº¡i KHÃ”NG train (máº·c Ä‘á»‹nh 8): Ä‘o tá»•ng quÃ¡t hoÃ¡, dá»«ng sá»›m, hiá»‡u chuáº©n T tháº­t
//   --aug <pháº§n trÄƒm>    % tá»« bá»‹ thay báº±ng "tá»« láº¡" trong view tÄƒng cÆ°á»ng (máº·c Ä‘á»‹nh 20)
//   --eval-every <N>     Ä‘o táº­p giá»¯-láº¡i má»—i N epoch (máº·c Ä‘á»‹nh 5)
//   --patience <N>       dá»«ng sá»›m sau N láº§n Ä‘o khÃ´ng cáº£i thiá»‡n, khÃ´i phá»¥c epoch tá»‘t nháº¥t (máº·c Ä‘á»‹nh 8)
//   --temp <T*100>       Ã©p nhiá»‡t Ä‘á»™ T cuá»‘i cÃ¹ng (vd 150 = 1.50)
//   --replay <tá»‰ lá»‡>     tá»‰ lá»‡ máº«u Ã´n táº­p trá»™n vÃ o khi --resume (máº·c Ä‘á»‹nh 0.15)
//   --replay-cap <N>     sá»‘ dÃ²ng tá»‘i Ä‘a cá»§a replay_buffer.txt (máº·c Ä‘á»‹nh 4096)
//   --replay-file <f>    tá»‡p bá»™ Ä‘á»‡m há»“i á»©c (máº·c Ä‘á»‹nh replay_buffer.txt)
//
// Tá»‡p .bin xuáº¥t ra lÃ  Ä‘á»‹nh dáº¡ng v16 (header 128 B + hash kiáº¿n trÃºc + CRC vÃ¹ng máº¡ng + chuá»—i mÃ´ táº£ huáº¥n luyá»‡n).
// File v9..v14 cÅ© KHÃ”NG náº¡p Ä‘Æ°á»£c -> train láº¡i.
#define BIT_LLM_TRAIN // báº­t trainer float + dataset (chá»‰ cáº§n khi huáº¥n luyá»‡n)
#include <iostream>
#include <fstream>
#include <chrono>
#include <string>
#include <vector>
#include <random>
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <limits>
#include "bit_llm_core.cpp" // unity build: nhÃºng tháº³ng lÃµi

using namespace std;

struct Options
{
    int epochs = 0;
    bool resume = false, saveState = true;
    string data = "dataset.txt", out = "bit_llm_weights.bin", state = "bit_llm_state.bin", replayFile = "replay_buffer.txt";
    double replayRatio = 0.15;
    size_t replayCap = 4096;
    double lr = 0.003;
    u32 neg = 24, hnEvery = 16, logq100 = 100;
    u32 holdout = 8, augPct = 20, evalEvery = 5, patience = 8, forceT = 0;
};

static bool all_digits(const string &s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (!isdigit((unsigned char)c))
            return false;
    return true;
}

static Options parse_args(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--resume")
            o.resume = true;
        else if (a == "--no-state")
            o.saveState = false;
        else if (a == "--data" && hasNext)
            o.data = argv[++i];
        else if (a == "--out" && hasNext)
            o.out = argv[++i];
        else if (a == "--state" && hasNext)
            o.state = argv[++i];
        else if (a == "--replay-file" && hasNext)
            o.replayFile = argv[++i];
        else if (a == "--replay" && hasNext)
            o.replayRatio = max(0.0, min(1.0, atof(argv[++i])));
        else if (a == "--replay-cap" && hasNext)
            o.replayCap = (size_t)max(0, atoi(argv[++i]));
        else if (a == "--lr" && hasNext)
            o.lr = max(1e-5, min(0.1, atof(argv[++i])));
        else if (a == "--neg" && hasNext)
            o.neg = (u32)max(1, min(64, atoi(argv[++i])));
        else if (a == "--hn-every" && hasNext)
            o.hnEvery = (u32)max(1, atoi(argv[++i]));
        else if (a == "--logq" && hasNext)
            o.logq100 = (u32)max(0.0, min(200.0, atof(argv[++i]) * 100.0));
        else if (a == "--holdout" && hasNext)
            o.holdout = (u32)max(0, min(50, atoi(argv[++i])));
        else if (a == "--aug" && hasNext)
            o.augPct = (u32)max(0, min(40, atoi(argv[++i])));
        else if (a == "--eval-every" && hasNext)
            o.evalEvery = (u32)max(1, atoi(argv[++i]));
        else if (a == "--patience" && hasNext)
            o.patience = (u32)max(1, atoi(argv[++i]));
        else if (a == "--temp" && hasNext)
            o.forceT = (u32)max(0, atoi(argv[++i]));
        else if (all_digits(a) && atoi(a.c_str()) > 0)
            o.epochs = atoi(a.c_str());
        else
            cerr << "[!] Bo qua tham so khong hieu: " << a << "\n";
    }
    if (o.epochs == 0)
        o.epochs = o.resume ? 40 : 100;
    return o;
}

static bool read_file(const string &path, string &out)
{
    ifstream f(path, ios::binary);
    if (!f.is_open())
        return false;
    f.seekg(0, ios::end);
    const streamoff sz = f.tellg();
    if (sz < 0 || (uint64_t)sz > (uint64_t)numeric_limits<size_t>::max())
        return false;
    f.seekg(0, ios::beg);
    out.resize((size_t)sz);
    if (sz > 0)
        f.read(&out[0], sz);
    return (bool)f;
}

// CÃ¡c dÃ²ng há»£p lá»‡ dáº¡ng "há»i | Ä‘Ã¡p" (bá» dÃ²ng trá»‘ng, dÃ²ng chÃº thÃ­ch '#')
static vector<string> get_pair_lines(const string &text)
{
    vector<string> v;
    size_t p = 0;
    while (p < text.size())
    {
        size_t e = text.find('\n', p);
        if (e == string::npos)
            e = text.size();
        string l = text.substr(p, e - p);
        p = e + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n'))
            l.pop_back();
        if (l.empty() || l[0] == '#')
            continue;
        if (l.find('|') == string::npos && l.find('\t') == string::npos)
            continue;
        v.push_back(l);
    }
    return v;
}

// Äáº©y vÄƒn báº£n dataset vÃ o lÃµi theo tá»«ng khá»‘i <= bá»™ Ä‘á»‡m (cáº¯t táº¡i ranh giá»›i dÃ²ng).
static u32 feed_dataset(const string &text, bool replay, bool reset)
{
    const size_t cap = wasm_bit_llm_get_data_size();
    char *dst = wasm_bit_llm_get_data_ptr();
    size_t pos = 0;
    u32 total = 0;
    bool first = true;
    while (pos < text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == string::npos)
            end = text.size();
        else
            ++end;
        if (end - pos > cap)
        {
            cerr << "[!] Bo qua dong dataset dai hon bo dem " << cap << " byte tai offset " << pos << ".\n";
            pos = end;
            continue;
        }
        size_t blockEnd = end;
        while (blockEnd < text.size())
        {
            size_t next = text.find('\n', blockEnd);
            if (next == string::npos)
                next = text.size();
            else
                ++next;
            if (next - pos > cap)
                break;
            blockEnd = next;
        }
        end = blockEnd;
        memcpy(dst, text.data() + pos, end - pos);
        total = (reset && first) ? wasm_bit_llm_load_dataset((u32)(end - pos)) : wasm_bit_llm_load_dataset_inc((u32)(end - pos), replay ? 1u : 0u);
        first = false;
        pos = end;
    }
    if (reset && first)
        total = wasm_bit_llm_load_dataset(0);
    return total;
}

// ---- Náº¡p .bin cÅ© qua chÃ­nh Ä‘Æ°á»ng náº¡p streaming (kiá»ƒm tra header + hash kiáº¿n trÃºc + CRC)
static bool load_model(const string &path)
{
    ifstream f(path, ios::binary);
    if (!f.is_open())
        return false;
    f.seekg(0, ios::end);
    const streamoff end = f.tellg();
    if (end < 0)
        return false;
    const size_t sz = (size_t)end;
    f.seekg(0, ios::beg);
    const u32 headerSize = wasm_bit_llm_begin_load();
    bool ok = sz >= headerSize;
    u8 *buf = wasm_bit_llm_get_chunk_buffer_ptr();
    const u32 mx = wasm_bit_llm_get_chunk_buffer_size();
    size_t off = 0, target = headerSize;
    while (ok && off < sz && off < target)
    {
        const u32 n = (u32)min<size_t>(min<size_t>(mx, target - off), sz - off);
        f.read((char *)buf, n);
        ok = (bool)f && wasm_bit_llm_load_chunk_direct((u32)off, n);
        off += n;
        if (ok && off == headerSize)
            target = (size_t)wasm_bit_llm_get_stream_bytes();
    }
    ok = ok && off == target;
    ok = ok && wasm_bit_llm_finalize_load() != 0;
    if (!ok)
        wasm_bit_llm_init();
    return ok;
}

// ---- Tráº¡ng thÃ¡i float (master) lÆ°u riÃªng, KHÃ”NG náº±m trong .bin suy luáº­n
struct StateHeader
{
    u32 magic, version, vocabCount, epochs, archHash, l1, lh, reserved;
};
constexpr u32 STATE_MAGIC = 0x31534E42u; // 'BNS1'
constexpr u32 STATE_VERSION = 1;

template <class T>
static void wr(ofstream &o, const T *p, size_t n) { o.write((const char *)p, (streamsize)(n * sizeof(T))); }
template <class T>
static bool rd(ifstream &f, T *p, size_t n)
{
    f.read((char *)p, (streamsize)(n * sizeof(T)));
    return (bool)f;
}

static bool save_state(const string &path)
{
    const u32 N = g_model.vocabCount;
    StateHeader sh{STATE_MAGIC, STATE_VERSION, N, g_model.epochs, ARCH_HASH, L1, LH, 0};
    ofstream o(path, ios::binary | ios::trunc);
    if (!o.is_open())
        return false;
    wr(o, &sh, 1);
    wr(o, T_E, (size_t)NF * L1);
    for (Dense *d : {&D_eb, &D_w1, &D_b1, &D_w2, &D_b2})
        wr(o, d->w, d->n);
    wr(o, T_hw, (size_t)N * LH);
    wr(o, T_hb, N);
    return (bool)o;
}

static bool load_state(const string &path)
{
    ifstream f(path, ios::binary);
    if (!f.is_open())
        return false;
    f.seekg(0, ios::end);
    const streamoff fileBytes = f.tellg();
    f.seekg(0, ios::beg);
    if (fileBytes < 0)
        return false;
    StateHeader sh{};
    if (!rd(f, &sh, 1) || sh.magic != STATE_MAGIC || sh.version != STATE_VERSION || sh.vocabCount != g_model.vocabCount || sh.epochs != g_model.epochs ||
        sh.archHash != ARCH_HASH || sh.l1 != L1 || sh.lh != LH)
        return false; // khÃ´ng khá»›p .bin -> dÃ¹ng báº£n dequantize tá»« .bin
    const u32 N = g_model.vocabCount;
    size_t expectedBytes = sizeof(StateHeader) + ((size_t)NF * L1 + (size_t)N * LH + N) * sizeof(float);
    for (Dense *d : {&D_eb, &D_w1, &D_b1, &D_w2, &D_b2})
        expectedBytes += (size_t)d->n * sizeof(float);
    if ((uint64_t)fileBytes != expectedBytes)
        return false;
    // Ä‘á»c vÃ o bá»™ Ä‘á»‡m táº¡m trÆ°á»›c, chá»‰ ghi Ä‘Ã¨ master khi Ä‘á»c Ä‘á»§ (trÃ¡nh ná»­a vá»i)
    vector<float> e((size_t)NF * L1), hw((size_t)N * LH), hb(N);
    vector<vector<float>> dn;
    bool ok = rd(f, e.data(), e.size());
    for (Dense *d : {&D_eb, &D_w1, &D_b1, &D_w2, &D_b2})
    {
        dn.emplace_back(d->n);
        ok = ok && rd(f, dn.back().data(), d->n);
    }
    ok = ok && rd(f, hw.data(), hw.size()) && rd(f, hb.data(), hb.size());
    if (!ok)
        return false;
    auto finite = [](const vector<float> &v) { return all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); }); };
    if (!finite(e) || !finite(hw) || !finite(hb))
        return false;
    for (const vector<float> &v : dn)
        if (!finite(v))
            return false;
    memcpy(T_E, e.data(), e.size() * 4);
    size_t k = 0;
    for (Dense *d : {&D_eb, &D_w1, &D_b1, &D_w2, &D_b2})
        memcpy(d->w, dn[k++].data(), d->n * 4);
    memcpy(T_hw, hw.data(), hw.size() * 4);
    memcpy(T_hb, hb.data(), hb.size() * 4);
    refresh_quant();
    return true;
}

// ---- Bá»™ Ä‘á»‡m há»“i á»©c (Micro-Replay): má»—i phiÃªn chiáº¿m pháº§n cá»¡ ngang nhau (cap/(sá»‘ phiÃªn + 1))
static void update_replay_buffer(const Options &o, const vector<string> &newLines, bool resumed, u64 seed)
{
    if (o.replayCap == 0)
        return;
    vector<string> oldLines;
    size_t sessions = 0;
    if (resumed)
    {
        string t;
        if (read_file(o.replayFile, t))
        {
            sscanf(t.c_str(), "# sessions=%zu", &sessions);
            oldLines = get_pair_lines(t);
        }
    }
    mt19937_64 rng(seed);
    auto sample = [&](vector<string> v, size_t k)
    {
        shuffle(v.begin(), v.end(), rng);
        if (v.size() > k)
            v.resize(k);
        return v;
    };
    const size_t newQuota = max<size_t>(1, o.replayCap / (sessions + 1));
    vector<string> ns = sample(newLines, newQuota);
    vector<string> os = sample(oldLines, o.replayCap > ns.size() ? o.replayCap - ns.size() : 0);
    ofstream out(o.replayFile, ios::binary | ios::trunc);
    out << "# sessions=" << (sessions + 1) << "\n";
    for (const string &l : os)
        out << l << "\n";
    for (const string &l : ns)
        out << l << "\n";
    cout << "[+] Cap nhat bo dem hoi uc: " << os.size() << " mau cu + " << ns.size() << " mau moi -> " << o.replayFile << "\n";
}

int main(int argc, char **argv)
{
    const Options opt = parse_args(argc, argv);
    cout << "[+] Khoi tao Bit-LLM v16 'Bit-NNUE' (embedding hoc duoc, thap NNUE QAT, LM-head i8, sampled-softmax + hard-negative)\n";
#ifdef _OPENMP
    cout << "[+] OpenMP: " << omp_get_max_threads() << " luong (song song hoa quet tu vung / hieu chuan T)\n";
#endif
    wasm_bit_llm_init();
    wasm_bit_llm_set_train_opts(0, opt.hnEvery, 0, 0, 1); // lr giáº£m tuyáº¿n tÃ­nh theo tá»•ng epoch
    wasm_bit_llm_set_holdout(opt.holdout * 10);           // pháº§n trÄƒm -> pháº§n nghÃ¬n; pháº£i Ä‘áº·t TRÆ¯á»šC khi náº¡p dataset
    wasm_bit_llm_set_aug(opt.augPct);
    wasm_bit_llm_set_hyper(0, (u32)(opt.lr * 1e5 + 0.5));
    wasm_bit_llm_set_hyper(1, opt.neg);
    wasm_bit_llm_set_hyper(2, opt.hnEvery);
    wasm_bit_llm_set_hyper(4, opt.logq100);

    // 1. Káº¿ thá»«a mÃ´ hÃ¬nh cÅ© (náº¿u --resume)
    bool resumed = false;
    if (opt.resume)
    {
        resumed = load_model(opt.out);
        if (!resumed)
            cerr << "[!] Khong nap duoc " << opt.out << " (khong ton tai / sai phien ban / hong) -> train moi tu dau.\n";
        else
        {
            cout << "[+] Da ke thua mo hinh: " << wasm_bit_llm_get_vocab_count() << " token, " << g_model.epochs << " epoch da train, T=" << calT() << ".\n";
            if (opt.saveState && load_state(opt.state))
                cout << "[+] Da nap trong so float tu " << opt.state << " (hoc tiep khong mat phan le luong tu hoa).\n";
            else
                cout << "[!] Khong co/khong khop file state -> dung trong so dequantize tu .bin (mat phan le luong tu hoa, van train tiep duoc).\n";
        }
    }
    if (!wasm_bit_llm_begin_session(resumed ? 1u : 0u, 0u))
    {
        cerr << "[-] Khong the bat dau phien huan luyen.\n";
        return 1;
    }

    // 2. Äá»c dataset má»›i (náº¡p theo khá»‘i)
    string text;
    if (!read_file(opt.data, text))
    {
        cerr << "[-] Khong tim thay file " << opt.data << "!\n";
        return 1;
    }
    cout << "[+] Dang nap dataset " << opt.data << " (" << text.size() << " bytes)...\n";
    const vector<string> newLines = get_pair_lines(text);
    const u32 vocabBefore = wasm_bit_llm_get_vocab_count();
    u32 pairs = feed_dataset(text, false, !resumed);
    cout << "[+] Da nap: " << pairs << " cap hoi/dap, " << wasm_bit_llm_get_vocab_count() << " token (" << (wasm_bit_llm_get_vocab_count() - vocabBefore)
         << " token moi), numDocs = " << g_model.numDocs << ".\n";
    cout << "    bo qua " << wasm_bit_llm_get_ds_stat(1) << " dong loi, loc " << wasm_bit_llm_get_ds_stat(5) << " dong suy bien (lap), tran tu vung "
         << wasm_bit_llm_get_ds_stat(4) << " (gioi han " << TRAIN_VOCAB_CAP << " token)\n";
    if (wasm_bit_llm_get_ds_stat(4) > 0)
        cerr << "[!] CANH BAO: tu vung vuot " << TRAIN_VOCAB_CAP << " -> cac cap chua tu moi bi bo. Hay build lai voi -DTRAIN_VOCAB_CAP=<lon hon>.\n";
    if (pairs == 0)
    {
        cerr << "[-] Dataset rong hoac khong dung dinh dang (cau hoi | dap an).\n";
        return 1;
    }

    // 3. Trá»™n máº«u Ã´n táº­p (Micro-Replay) tá»« cÃ¡c dataset cÅ©
    if (resumed && opt.replayRatio > 0.0)
    {
        string rt;
        if (read_file(opt.replayFile, rt))
        {
            vector<string> rl = get_pair_lines(rt);
            mt19937_64 rng(0xB17ULL ^ ((u64)g_model.epochs << 20));
            shuffle(rl.begin(), rl.end(), rng);
            const size_t room = pairs < MAX_PAIRS ? MAX_PAIRS - pairs : 0;
            size_t want = (size_t)(opt.replayRatio * pairs + 0.999);
            want = min(want, min(rl.size(), room));
            if (want > 0)
            {
                string joined;
                for (size_t i = 0; i < want; ++i)
                {
                    joined += rl[i];
                    joined += '\n';
                }
                const u32 before = pairs;
                pairs = feed_dataset(joined, true, false);
                cout << "[+] Tron " << (pairs - before) << " mau on tap (replay), " << (int)(100.0 * (pairs - before) / max<u32>(1, pairs)) << "% tong so cap.\n";
            }
            else
                cout << "[!] Khong con cho cho mau on tap (gioi han " << MAX_PAIRS << " cap).\n";
        }
        else
            cout << "[!] Chua co " << opt.replayFile << " -> khong co mau on tap.\n";
    }

    // 4. Train nhiá»u epoch
    wasm_bit_llm_set_hyper(3, (u32)opt.epochs); // tá»•ng epoch cho lá»‹ch lr
    const u32 nHold = wasm_bit_llm_get_holdout_count();
    const bool useVal = nHold >= 8;
    cout << "[+] Bat dau " << (resumed ? "HOC LIEN TUC " : "") << "training " << opt.epochs << " epoch (" << pairs << " cap, lr=" << opt.lr << ", neg=" << opt.neg
         << ", hn-every=" << opt.hnEvery << ", logq=" << opt.logq100 / 100.0 << ", aug=" << opt.augPct << "%).\n";
    cout << "[+] Tap giu-lai: " << nHold << " cap (" << (useVal ? "dung de hieu chuan T that + dung som + chon epoch tot nhat" : "QUA IT -> khong co kiem dinh; T hieu chuan tren mau train") << ").\n";

    auto t0 = chrono::steady_clock::now();
    double bestValNll = numeric_limits<double>::infinity();
    int bestEp = 0, sinceBest = 0, lastEp = 0;
    bool haveBest = false;
    for (int ep = 1; ep <= opt.epochs; ++ep)
    {
        lastEp = ep;
        const auto te = chrono::steady_clock::now();
        const u32 steps = wasm_bit_llm_train_epoch((u32)ep + g_model.epochs);
        const double sec = chrono::duration<double>(chrono::steady_clock::now() - te).count();
        if (steps == 0)
        {
            cerr << "[-] Epoch khong co buoc nao (tat ca cap deu la giu-lai?).\n";
            break;
        }
        if (ep % 10 == 0)
            wasm_bit_llm_calibrate_temperature((u32)ep);

        if (ep == 1 || ep % 5 == 0 || ep == opt.epochs)
        {
            const double eta = (chrono::duration<double>(chrono::steady_clock::now() - t0).count() / ep) * (opt.epochs - ep);
            printf("Epoch %d/%d | loss %.3f | hn-top1 %.1f%% | lr %.5f | %.1fs/epoch (%.0f buoc/s) | con ~%.0fs\n", ep, opt.epochs, wasm_bit_llm_get_stat(0) / 1000.0,
                   wasm_bit_llm_get_stat(1) / 10.0, wasm_bit_llm_get_stat(5) / 1e6, sec, steps / max(sec, 1e-9), eta);
#ifdef BIT_LLM_DIAGNOSTICS
            printf("  [diag] clip-active acc %.1f%% | fc1 %.1f%% | fc2 %.1f%% | dacc-zero %.1f%% | mean|dacc| %.6g | mean-logQ-shift %.5f\n",
                   wasm_bit_llm_get_train_diag(0), wasm_bit_llm_get_train_diag(1), wasm_bit_llm_get_train_diag(2),
                   wasm_bit_llm_get_train_diag(3), wasm_bit_llm_get_train_diag(4), wasm_bit_llm_get_train_diag(5));
#endif
            fflush(stdout);
        }
        if (useVal && ep % (int)opt.evalEvery == 0)
        {
            const u32 v0 = wasm_bit_llm_eval_holdout(0);
            const double nll0 = wasm_bit_llm_get_eval_nll();
            const u32 v1 = wasm_bit_llm_eval_holdout(1);
            const double valNll = 0.5 * (nll0 + wasm_bit_llm_get_eval_nll());
            printf("  [val] Epoch %d | top-1: %.1f%% / %.1f%% | NLL %.4f | T=%.2f\n", ep, v0 / 10.0f, v1 / 10.0f, valNll, calT());
            fflush(stdout);
            if (valNll + 1e-4 < bestValNll)
            {
                bestValNll = valNll;
                bestEp = ep;
                sinceBest = 0;
                haveBest = wasm_bit_llm_snapshot_best() != 0;
            }
            else if (++sinceBest >= (int)opt.patience)
            {
                cout << "[+] Dung som tai epoch " << ep << ": NLL tap giu-lai khong tot hon epoch " << bestEp << ".\n";
                break;
            }
        }
    }
    if (useVal && haveBest && bestEp != lastEp)
        if (wasm_bit_llm_restore_best())
            cout << "[+] Khoi phuc trong so epoch tot nhat tren tap giu-lai: epoch " << bestEp << " (NLL " << bestValNll << ").\n";
    const auto dur = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - t0).count();
    cout << "[+] Hoan tat training trong " << dur << " giay! Mat do trong so head khac 0: " << (float)wasm_bit_llm_weight_density() / 10.0f << "%\n";

    if (opt.forceT)
        wasm_bit_llm_set_temp(opt.forceT);
    else
        wasm_bit_llm_calibrate_temperature(9999); // T cuá»‘i cÃ¹ng Ä‘i vÃ o header (Ä‘o trÃªn táº­p giá»¯-láº¡i náº¿u cÃ³)
    cout << "[+] Nhiet do hieu chuan T = " << calT() << " | NLL = " << wasm_bit_llm_get_stat(2) / 1000.0 << "\n";

    // 5. Xuáº¥t file v16 (kÃ¨m chuá»—i mÃ´ táº£ huáº¥n luyá»‡n Ä‘á»ƒ biáº¿t mÃ´ hÃ¬nh Ä‘Æ°á»£c train tháº¿ nÃ o)
    {
        char desc[512];
        const int dl = snprintf(desc, sizeof(desc), "bit-llm v16 | data=%s | pairs=%u | epochs_total=%u | lr=%.4f neg=%u hn=%u logq=%.2f aug=%u%% hold=%u%% | T=%.2f | %s", opt.data.c_str(),
                                pairs, g_model.epochs, opt.lr, opt.neg, opt.hnEvery, opt.logq100 / 100.0, opt.augPct, opt.holdout, calT(), resumed ? "continual" : "fresh");
        const u32 n = (u32)min<int>(dl, (int)sizeof(desc) - 1);
        memcpy(wasm_bit_llm_get_input_ptr(), desc, n);
        wasm_bit_llm_set_desc(n);

        const u32 total = wasm_bit_llm_export_begin();
        ofstream out(opt.out, ios::binary | ios::trunc);
        u32 off = 0;
        for (; off < total;)
        {
            const u32 n2 = wasm_bit_llm_export_read(off, wasm_bit_llm_get_chunk_buffer_size());
            if (n2 == 0)
                break;
            out.write((const char *)wasm_bit_llm_get_chunk_buffer_ptr(), n2);
            if (!out)
                break;
            off += n2;
        }
        out.flush();
        if (!out || off != total)
        {
            cerr << "[-] Ghi " << opt.out << " that bai!\n";
            return 1;
        }
    }
    cout << "[+] Da luu mo hinh (" << wasm_bit_llm_get_model_size() / 1024.0 / 1024.0 << " MB, " << wasm_bit_llm_get_vocab_count() << " token, CRC " << std::hex << g_model.netCrc
         << std::dec << ") vao: " << opt.out << "\n";

    // 6. LÆ°u state float + cáº­p nháº­t bá»™ Ä‘á»‡m há»“i á»©c
    if (opt.saveState)
    {
        if (save_state(opt.state))
            cout << "[+] Da luu trong so float vao " << opt.state << " (chi de train tiep).\n";
        else
            cerr << "[!] Khong ghi duoc " << opt.state << "\n";
    }
    {
        vector<string> keepLines;
        for (const string &l : newLines)
        {
            const size_t n = min<size_t>(l.size(), IO_BUFFER_SIZE - 1);
            memcpy(wasm_bit_llm_get_input_ptr(), l.data(), n);
            if (!wasm_bit_llm_is_holdout_line((u32)n))
                keepLines.push_back(l);
        }
        update_replay_buffer(opt, keepLines, resumed, 0x5EEDULL ^ ((u64)g_model.epochs << 16));
    }
    wasm_bit_llm_end_session();
    return 0;
}

