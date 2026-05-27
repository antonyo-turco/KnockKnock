#!/usr/bin/env python3
"""
antitheft_analysis.py
═══════════════════════════════════════════════════════════════════════════════
Analisi completa dei dati raccolti dall'accelerometro ADXL362 per il sistema
antifurto TinyML su ESP32-C3.

Riproduce fedelmente la pipeline del firmware C++ (config.h / tinyml_training.h
/ feature_extraction.cpp) e genera 10 grafici per motivare le scelte di design.

Grafici prodotti
────────────────
  fig1_raw_signal.png               Segnale grezzo X/Y/Z (primi 10s)
  fig2a_sampling_rate_per_axis.png  Spettro per asse × frequenza (griglia 4×4)
  fig2b_band2040_energy_per_axis.png Energia 20-40Hz per asse — riassunto quantitativo
  fig3_window_size.png              Stabilità feature al variare della window size
  fig4_pca_k4_vs_k10.png            PCA 2D K=4 vs K=10
  fig5_stats_k4_vs_k10.png          Statistiche cluster K=4 vs K=10
  fig6_distance_distributions.png   Distribuzioni distanze + soglie (K=10)
  fig7_fp_vs_sigma.png              FP rate vs SIGMA_MULT per K=4 e K=10
  fig8_novelty_buffer.png           Riempimento novelty buffer nel tempo
  fig9_feature_correlation.png      Matrice correlazione top-20 feature

Input : CSV senza header, colonne  millis,x,y,z  (campionato a 400 Hz)
         Opzionale: un secondo CSV per analisi comparativa tra ambienti
Output: PNG salvati in OUT_DIR

Uso
───
    python antitheft_analysis.py                              # usa il CSV di default
    python antitheft_analysis.py --csv mio_file.csv
    python antitheft_analysis.py --csv data.csv --out ./grafici/
    python antitheft_analysis.py --csv roma.csv --csv2 rocca.csv --label1 Roma --label2 Rocca

Dipendenze
──────────
    pip install numpy pandas matplotlib scikit-learn scipy
"""

import argparse
import os
import warnings
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Ellipse
from sklearn.decomposition import PCA

warnings.filterwarnings("ignore")

# ══════════════════════════════════════════════════════════════════════════════
#  PARAMETRI  —  specchio esatto di config.h / tinyml_training.h
# ══════════════════════════════════════════════════════════════════════════════
RAW_RATE_HZ      = 400.0   # frequenza di campionamento del CSV
SAMPLE_RATE_HZ   = 100.0   # frequenza usata dal firmware
SAMPLE_COUNT     = 256     # campioni per finestra
FFT_SIZE         = 256
TOP7_COUNT       = 7
FEATURE_DIM      = 47
NOVELTY_BUF_SIZE = 200
KMEANS_K         = 10
NOVELTY_THRESH   = 1.5
SIGMA_MULT       = 3.0
MAX_DIST_MARGIN  = 1.1
MIN_THRESHOLD    = 0.30

FEAT_NAMES = [
    "impact_score",
    "m_p99","x_p99","y_p99","z_p99",
    "m_jerk","x_jerk","y_jerk","z_jerk",
    "m_b2040","x_b2040","y_b2040","z_b2040",
    "m_b1_5","x_b1_5","y_b1_5","z_b1_5",
    "m_b5_20","x_b5_20","y_b5_20","z_b5_20",
    "x_zcr","y_zcr","z_zcr",
    "x_f0","x_f1","x_f2","x_f3","x_f4","x_f5","x_f6",
    "y_f0","y_f1","y_f2","y_f3","y_f4","y_f5","y_f6",
    "z_f0","z_f1","z_f2","z_f3","z_f4","z_f5","z_f6",
    "time_sin","time_cos",
]

plt.rcParams.update({
    "figure.facecolor":   "white",
    "axes.facecolor":     "#f8f8f8",
    "axes.grid":          True,
    "grid.alpha":         0.4,
    "font.size":          11,
    "axes.spines.top":    False,
    "axes.spines.right":  False,
})

# ══════════════════════════════════════════════════════════════════════════════
#  FEATURE EXTRACTION  —  replica esatta del C++
# ══════════════════════════════════════════════════════════════════════════════

def percentile99(arr):
    return float(np.percentile(np.abs(arr), 99))

def jerk_max(arr):
    return float(np.max(np.abs(np.diff(arr))))

def compute_zcr(arr):
    s = np.sign(arr); s[s == 0] = 1
    return float(np.sum(s[1:] != s[:-1])) / (len(arr) - 1)

def bandpower(freqs, magnitudes, low, high):
    mask = (freqs >= low) & (freqs < high)
    if mask.sum() < 2:
        return 0.0
    return float(np.trapezoid(magnitudes[mask], freqs[mask]))

def top7_freqs(magnitudes, sr):
    n     = len(magnitudes)
    freqs = np.fft.rfftfreq(n * 2, d=1.0 / sr)[:n]
    idx   = np.argsort(magnitudes[1:])[-TOP7_COUNT:] + 1
    top_f = np.sort(freqs[idx])
    if len(top_f) < TOP7_COUNT:
        top_f = np.pad(top_f, (0, TOP7_COUNT - len(top_f)))
    return top_f

def signal_metrics(signal, sr):
    p99  = percentile99(signal)
    jmax = jerk_max(signal)
    win  = np.hamming(len(signal))
    fft_m = np.abs(np.fft.rfft(signal * win))[1:]
    freqs = np.fft.rfftfreq(len(signal), d=1.0 / sr)[1:]
    b15   = bandpower(freqs, fft_m,  1.0,  5.0)
    b520  = bandpower(freqs, fft_m,  5.0, 20.0)
    b2040 = bandpower(freqs, fft_m, 20.0, 40.0)
    t7    = top7_freqs(fft_m, sr)
    return p99, jmax, b15, b520, b2040, t7

def compute_features(x, y, z, t_ms, sr=100.0):
    mag   = np.sqrt(x**2 + y**2 + z**2)
    m_rms = float(np.sqrt(np.mean(mag**2)))

    mp99, mj, mb15, mb520, mb2040, mt7 = signal_metrics(mag, sr)
    xp99, xj, xb15, xb520, xb2040, xt7 = signal_metrics(x,   sr)
    yp99, yj, yb15, yb520, yb2040, yt7 = signal_metrics(y,   sr)
    zp99, zj, zb15, zb520, zb2040, zt7 = signal_metrics(z,   sr)

    x_zcr = compute_zcr(x); y_zcr = compute_zcr(y); z_zcr = compute_zcr(z)

    p99_n  = min(1.0, mp99 / (m_rms *  8.0 + 1e-9))
    jrk_n  = min(1.0, float(np.max(np.abs(np.diff(mag)))) / (m_rms * 12.0 + 1e-9))
    bnd_n  = min(1.0, mb2040 / (m_rms**2 * len(x) * 0.3 + 1e-9))
    impact = 0.40*p99_n + 0.35*jrk_n + 0.25*bnd_n

    minute = ((t_ms / 1000.0) % 86400) / 60.0
    angle  = 2 * np.pi * float(np.mean(minute)) / 1440.0

    return np.array([
        impact,
        mp99, xp99, yp99, zp99,
        mj,   xj,   yj,   zj,
        mb2040, xb2040, yb2040, zb2040,
        mb15,   xb15,   yb15,   zb15,
        mb520,  xb520,  yb520,  zb520,
        x_zcr, y_zcr, z_zcr,
        *xt7, *yt7, *zt7,
        np.sin(angle), np.cos(angle),
    ], dtype=np.float32)

# ══════════════════════════════════════════════════════════════════════════════
#  K-MEANS++  —  replica esatta del C++ (deterministico)
# ══════════════════════════════════════════════════════════════════════════════

def run_kmeans(features_norm, nov_buf, K):
    # Seeding K++ deterministico (furthest-point)
    centroids = [nov_buf[0].copy()]
    for _ in range(1, K):
        d = np.array([min(np.linalg.norm(p - c) for c in centroids) for p in nov_buf])
        centroids.append(nov_buf[np.argmax(d)].copy())
    centroids  = np.array(centroids, dtype=np.float32)

    counts     = np.ones(K)
    dist_mean  = np.zeros(K)
    dist_M2    = np.zeros(K)
    dist_max   = np.zeros(K)
    dist_n     = np.zeros(K)
    assign     = []
    dists_all  = []

    for fv in features_norm:
        d_all = np.linalg.norm(centroids - fv, axis=1)
        k     = int(np.argmin(d_all))
        d     = float(d_all[k])
        counts[k]    += 1
        centroids[k] += (fv - centroids[k]) / counts[k]
        dist_n[k]    += 1
        delta         = d - dist_mean[k]
        dist_mean[k] += delta / dist_n[k]
        dist_M2[k]   += delta * (d - dist_mean[k])
        if d > dist_max[k]:
            dist_max[k] = d
        assign.append(k)
        dists_all.append(d)

    dist_sigma = np.sqrt(np.maximum(dist_M2 / np.maximum(dist_n - 1, 1), 0))
    thresholds = np.maximum(
        np.maximum(dist_mean + SIGMA_MULT * dist_sigma,
                   dist_max  * MAX_DIST_MARGIN),
        MIN_THRESHOLD,
    )
    return {
        "centroids":      centroids,
        "counts":         counts,
        "assign":         np.array(assign),
        "dists":          np.array(dists_all),
        "dist_mean":      dist_mean,
        "dist_sigma":     dist_sigma,
        "dist_max":       dist_max,
        "dist_threshold": thresholds,
        "thr":            thresholds,
        "dist_n":         dist_n,
    }

# ══════════════════════════════════════════════════════════════════════════════
#  HELPER GRAFICO: ellisse 1σ attorno a un cluster
# ══════════════════════════════════════════════════════════════════════════════

def draw_ellipse(ax, pts, center, color):
    if len(pts) < 3:
        return
    cov  = np.cov(pts.T)
    vals, vecs = np.linalg.eigh(cov)
    vals = np.maximum(vals, 0)
    angle = np.degrees(np.arctan2(*vecs[:, 1][::-1]))
    w, h  = 2 * np.sqrt(vals)
    ax.add_patch(Ellipse(center, w, h, angle=angle,
                         color=color, alpha=0.10, zorder=1))
    ax.add_patch(Ellipse(center, w, h, angle=angle,
                         fill=False, edgecolor=color,
                         alpha=0.50, lw=1.5, ls="--", zorder=2))

# ══════════════════════════════════════════════════════════════════════════════
#  MAIN
# ══════════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv",    default="captured_data.csv",
                        help="CSV principale (millis,x,y,z senza header)")
    parser.add_argument("--csv2",   default="",
                        help="CSV secondario per analisi comparativa (opzionale)")
    parser.add_argument("--label1", default="Ambiente 1",
                        help="Etichetta per --csv")
    parser.add_argument("--label2", default="Ambiente 2",
                        help="Etichetta per --csv2")
    parser.add_argument("--out",    default="./",
                        help="Directory di output per i PNG")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    OUT = args.out.rstrip("/") + "/"

    # ── 1. Caricamento ────────────────────────────────────────────────────────
    print("═" * 60)
    print("  CARICAMENTO DATI")
    print("═" * 60)
    df = pd.read_csv(args.csv, header=None, names=["millis", "x", "y", "z"])
    dt_med = df["millis"].diff().dropna().median()
    eff_hz = 1000.0 / dt_med
    print(f"  Campioni  : {len(df):,}")
    print(f"  Durata    : {(df.millis.iloc[-1]-df.millis.iloc[0])/1000:.1f} s")
    print(f"  Freq eff. : {eff_hz:.1f} Hz  (Δt mediano={dt_med:.2f} ms)")

    raw_x = df["x"].values.astype(np.float32)
    raw_y = df["y"].values.astype(np.float32)
    raw_z = df["z"].values.astype(np.float32)
    raw_t = df["millis"].values.astype(np.float64)

    # ── 2. Downsample 400 → 100 Hz ────────────────────────────────────────────
    ds   = int(round(RAW_RATE_HZ / SAMPLE_RATE_HZ))
    x100 = raw_x[::ds]; y100 = raw_y[::ds]
    z100 = raw_z[::ds]; t100 = raw_t[::ds]

    # ── 3. Feature extraction ─────────────────────────────────────────────────
    print("\n  FEATURE EXTRACTION (100 Hz, window=256)")
    n_windows    = len(x100) // SAMPLE_COUNT
    features_list = []
    for i in range(n_windows):
        s = i * SAMPLE_COUNT; e = s + SAMPLE_COUNT
        features_list.append(
            compute_features(x100[s:e], y100[s:e], z100[s:e], t100[s:e])
        )
    features = np.array(features_list)
    print(f"  Finestre  : {len(features)}")

    # ── 4. Normalizzazione Welford ────────────────────────────────────────────
    feat_mean = features.mean(axis=0)
    feat_std  = features.std(axis=0)
    feat_std[feat_std < 1e-9] = 1.0
    features_norm = (features - feat_mean) / feat_std

    # ── 5. Novelty buffer ─────────────────────────────────────────────────────
    print("\n  NOVELTY BUFFER")
    nov_buf  = []
    nov_fill = []      # traccia il riempimento nel tempo
    for fv in features_norm:
        if len(nov_buf) < 10:          # KMEANS_K=10 come nel firmware
            nov_buf.append(fv.copy())
        else:
            d = np.linalg.norm(np.array(nov_buf) - fv, axis=1)
            near_d, near_i = d.min(), d.argmin()
            if near_d >= NOVELTY_THRESH:
                if len(nov_buf) < NOVELTY_BUF_SIZE:
                    nov_buf.append(fv.copy())
                else:
                    nov_buf[near_i] = fv.copy()
        nov_fill.append(len(nov_buf))
    nov_buf = np.array(nov_buf)
    print(f"  Campioni  : {len(nov_buf)}/{NOVELTY_BUF_SIZE}")

    # ── 6. K-Means per K=4 e K=10 ────────────────────────────────────────────
    print("\n  K-MEANS++")
    r4  = run_kmeans(features_norm, nov_buf, 4)
    r10 = run_kmeans(features_norm, nov_buf, 10)

    for label, r, K in [("K=4", r4, 4), ("K=10", r10, 10)]:
        print(f"\n  ── {label} {'─'*44}")
        print(f"  {'C':<4} {'n':>6} {'mean_d':>8} {'sigma':>8} {'max_d':>8} {'thr':>8}")
        for k in range(K):
            print(f"  C{k:<3} {int(r['counts'][k]):>6} {r['dist_mean'][k]:>8.4f} "
                  f"{r['dist_sigma'][k]:>8.4f} {r['dist_max'][k]:>8.4f} "
                  f"{r['dist_threshold'][k]:>8.4f}")
        deg = sum(1 for k in range(K) if r["counts"][k] <= 2)
        dom = r["counts"].max() / len(features_norm) * 100
        print(f"  Cluster degeneri: {deg}/{K}   Cluster dominante: {dom:.1f}%")

    # PCA condivisa
    pca     = PCA(n_components=2)
    proj    = pca.fit_transform(features_norm)
    var     = pca.explained_variance_ratio_
    P4      = pca.transform(r4["centroids"])
    P10     = pca.transform(r10["centroids"])
    PAL4    = plt.cm.Set1(np.linspace(0, 0.8, 4))
    PAL10   = plt.cm.tab10(np.linspace(0, 1, 10))

    # ══════════════════════════════════════════════════════════════════════════
    #  FIGURE
    # ══════════════════════════════════════════════════════════════════════════
    print("\n  GENERAZIONE GRAFICI")

    # ── Fig 1: Segnale grezzo ─────────────────────────────────────────────────
    fig, axes = plt.subplots(3, 1, figsize=(14, 7), sharex=True)
    fig.suptitle("Segnale grezzo ADXL362  —  primi 10 secondi  (400 Hz)",
                 fontsize=14, fontweight="bold")
    n10 = int(RAW_RATE_HZ * 10); t0 = raw_t[0]
    for ax, sig, lbl, col in zip(axes,
                                  [raw_x, raw_y, raw_z], ["X","Y","Z"],
                                  ["#e63946","#457b9d","#2a9d8f"]):
        ax.plot((raw_t[:n10]-t0)/1000, sig[:n10], color=col, lw=0.8)
        ax.set_ylabel(f"Asse {lbl}  [LSB]", fontsize=10)
    axes[-1].set_xlabel("Tempo [s]")
    fig.tight_layout()
    fig.savefig(OUT+"fig1_raw_signal.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig1_raw_signal.png")

    # ── Fig 2a: Confronto sampling rate FFT — griglia 4 assi × 4 frequenze ─────
    seg_start  = int(RAW_RATE_HZ * 5)
    seg_raw    = {
        "X":     raw_x[seg_start:seg_start+400].astype(np.float32),
        "Y":     raw_y[seg_start:seg_start+400].astype(np.float32),
        "Z":     raw_z[seg_start:seg_start+400].astype(np.float32),
        "|XYZ|": np.sqrt(raw_x[seg_start:seg_start+400]**2 +
                         raw_y[seg_start:seg_start+400]**2 +
                         raw_z[seg_start:seg_start+400]**2).astype(np.float32),
    }
    sampling_rates = [400, 200, 100, 50]
    axis_labels    = list(seg_raw.keys())
    cols_sr        = ["#264653","#2a9d8f","#e9c46a","#e76f51"]
    bands          = [(1,5,"1-5Hz"),(5,20,"5-20Hz"),(20,40,"20-40Hz")]

    fig, axes = plt.subplots(4, 4, figsize=(20, 16), sharey="row")
    fig.suptitle(
        "Effetto della frequenza di campionamento sullo spettro — per asse\n"
        "(stesso segmento fisico, 1 secondo a 400 Hz downsampledato)",
        fontsize=15, fontweight="bold", y=1.01,
    )
    for row, axis_name in enumerate(axis_labels):
        base = seg_raw[axis_name]
        for col, (sr, col_c) in enumerate(zip(sampling_rates, cols_sr)):
            ax    = axes[row][col]
            ds    = int(RAW_RATE_HZ // sr)
            sig   = base[::ds]
            n     = len(sig)
            fft_m = np.abs(np.fft.rfft(sig * np.hamming(n)))
            freqs = np.fft.rfftfreq(n, d=1.0/sr)
            ax.fill_between(freqs[1:], fft_m[1:], alpha=0.35, color=col_c)
            ax.plot(freqs[1:], fft_m[1:], color=col_c, lw=1.5)
            for lo, hi, _ in bands:
                if hi <= sr/2:
                    ax.axvspan(lo, hi, alpha=0.13, color="gray", zorder=0)
            ax.axvline(sr/2, color="red", lw=1.2, ls="--", alpha=0.75,
                       label=f"Nyquist={sr//2}Hz")
            # Evidenzia banda 20-40Hz
            if sr >= 80:
                mask_b = (freqs >= 20) & (freqs <= 40)
                if mask_b.any():
                    ax.fill_between(freqs[mask_b], fft_m[mask_b],
                                    alpha=0.45, color="#e63946", zorder=2,
                                    label="Banda 20-40Hz")
            else:
                ylim_now = ax.get_ylim()[1] or 1
                ax.annotate("banda\n20-40Hz\nTAGLIATA",
                            xy=(sr/2*0.75, ylim_now*0.5),
                            fontsize=8, color="red", ha="center",
                            bbox=dict(boxstyle="round,pad=0.2",
                                      fc="white", ec="red", alpha=0.8))
            ax.set_xlim(0, min(55, sr/2 + 3))
            ax.tick_params(labelsize=8)
            if row == 0:
                ax.set_title(f"{sr} Hz\n({n} smp/win)",
                             fontsize=11, fontweight="bold", color=col_c)
            if col == 0:
                ax.set_ylabel(f"Asse {axis_name}\nMagn. FFT", fontsize=10)
            if row == 3:
                ax.set_xlabel("Frequenza [Hz]", fontsize=9)
            ax.legend(fontsize=7, loc="upper right")
    fig.tight_layout()
    fig.savefig(OUT+"fig2a_sampling_rate_per_axis.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig2a_sampling_rate_per_axis.png")

    # ── Fig 2b: Energia banda 20-40Hz per asse — riassunto quantitativo ───────
    fig, axes = plt.subplots(1, 4, figsize=(16, 5))
    fig.suptitle(
        "Energia nella banda 20-40 Hz per asse al variare della frequenza di campionamento\n"
        "(la banda critica per rilevare impatti su vetro scompare sotto 80 Hz)",
        fontsize=13, fontweight="bold",
    )
    for col, axis_name in enumerate(axis_labels):
        ax       = axes[col]
        base     = seg_raw[axis_name]
        energies = []
        for sr in sampling_rates:
            ds    = int(RAW_RATE_HZ // sr)
            sig   = base[::ds]; n = len(sig)
            fft_m = np.abs(np.fft.rfft(sig * np.hamming(n)))
            freqs = np.fft.rfftfreq(n, d=1.0/sr)
            mask_b = (freqs >= 20) & (freqs < 40)
            e = float(np.trapezoid(fft_m[mask_b], freqs[mask_b])) \
                if mask_b.sum() >= 2 else 0.0
            energies.append(e)
        bar_colors = ["#e63946" if sr == 50 else
                      ("#e9c46a" if sr == 100 else "#2a9d8f")
                      for sr in sampling_rates]
        bars = ax.bar([f"{sr}Hz" for sr in sampling_rates], energies,
                      color=bar_colors, edgecolor="white", width=0.6)
        ax.set_title(f"Asse {axis_name}", fontweight="bold")
        ax.set_ylabel("Energia 20-40 Hz"); ax.set_xlabel("Freq. campionamento")
        ref = energies[sampling_rates.index(100)]
        if ref > 0:
            ax.axhline(ref, color="navy", lw=1.2, ls="--", alpha=0.5,
                       label="riferimento 100 Hz")
            ax.legend(fontsize=8)
        for bar, e in zip(bars, energies):
            label = f"{e:.0f}" if e > 0 else "0\n(tagliata)"
            ax.text(bar.get_x()+bar.get_width()/2,
                    bar.get_height() + max(energies)*0.02,
                    label, ha="center", va="bottom", fontsize=9,
                    color="red" if e == 0 else "black")
    fig.tight_layout()
    fig.savefig(OUT+"fig2b_band2040_energy_per_axis.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig2b_band2040_energy_per_axis.png")

    # ── Fig 3: Stabilità feature vs window size ───────────────────────────────
    window_sizes  = [32, 64, 128, 256, 512]
    feat_labels   = ["impact_score","m_p99","m_jerk_max","m_band_20_40"]
    feat_indices  = [0, 1, 5, 9]
    cv_results    = {}
    for ws in window_sizes:
        buf = []
        for i in range(min(len(x100)//ws, 300)):
            s = i*ws; e = s+ws
            buf.append(compute_features(x100[s:e], y100[s:e], z100[s:e], t100[s:e]))
        buf = np.array(buf)
        cv_results[ws] = [buf[:,fi].std()/(buf[:,fi].mean()+1e-9)
                          for fi in feat_indices]

    fig, axes = plt.subplots(1, 4, figsize=(16, 5))
    fig.suptitle("Stabilità delle feature al variare della durata della finestra\n"
                 "(CV = σ/μ — più basso = più stabile)",
                 fontsize=13, fontweight="bold")
    labels_x = [f"{ws/100:.2f}s\n({ws}smp)" for ws in window_sizes]
    for ax, fi, fname in zip(axes, range(4), feat_labels):
        cvs  = [cv_results[ws][fi] for ws in window_sizes]
        bars = ax.bar(labels_x, cvs,
                      color=["#e63946" if ws==256 else "#e9c46a"
                             for ws in window_sizes],
                      edgecolor="white", width=0.6)
        ax.set_title(fname, fontweight="bold")
        ax.set_ylabel("CV (σ/μ)"); ax.set_xlabel("Durata finestra")
        for bar, cv in zip(bars, cvs):
            ax.text(bar.get_x()+bar.get_width()/2,
                    bar.get_height()+0.002,
                    f"{cv:.3f}", ha="center", va="bottom", fontsize=9)
    fig.tight_layout()
    fig.savefig(OUT+"fig3_window_size.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig3_window_size.png")

    # ── Fig 4: PCA K=4 vs K=10 side-by-side ──────────────────────────────────
    def draw_cluster_panel(ax, proj, result, K, palette, title):
        for k in range(K):
            mask = result["assign"] == k
            ax.scatter(proj[mask,0], proj[mask,1],
                       color=palette[k], alpha=0.30, s=18,
                       label=f"C{k} n={mask.sum()}")
            if mask.sum() >= 3:
                draw_ellipse(ax, proj[mask],
                             pca.transform(result["centroids"][[k]])[0],
                             palette[k])
        c_proj = pca.transform(result["centroids"])
        ax.scatter(c_proj[:,0], c_proj[:,1], s=260, marker="*", zorder=6,
                   c=[palette[k] for k in range(K)],
                   edgecolors="black", linewidths=1.2)
        for k in range(K):
            ax.annotate(f"C{k}", c_proj[k],
                        textcoords="offset points", xytext=(6,5),
                        fontsize=9, fontweight="bold", color=palette[k])
        ax.set_title(title, fontsize=13, fontweight="bold")
        ax.set_xlabel(f"PC1 ({var[0]*100:.1f}%)")
        ax.set_ylabel(f"PC2 ({var[1]*100:.1f}%)")
        ax.legend(fontsize=8, ncol=2, loc="upper right")

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(18, 8), sharey=True)
    fig.suptitle("K=4  vs  K=10  —  Proiezione PCA 2D\n"
                 f"(varianza spiegata: {sum(var)*100:.1f}%)",
                 fontsize=14, fontweight="bold")
    draw_cluster_panel(ax1, proj, r4,  4,  PAL4,  "K = 4")
    draw_cluster_panel(ax2, proj, r10, 10, PAL10, "K = 10")
    fig.tight_layout()
    fig.savefig(OUT+"fig4_pca_k4_vs_k10.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig4_pca_k4_vs_k10.png")

    # ── Fig 5: Statistiche cluster K=4 vs K=10 ───────────────────────────────
    fig, axes = plt.subplots(2, 2, figsize=(15, 10))
    fig.suptitle("K=4 vs K=10  —  Statistiche per cluster",
                 fontsize=14, fontweight="bold")

    def bar_counts(ax, r, K, palette, title):
        ks = np.arange(K)
        ax.bar(ks, r["counts"], color=[palette[k] for k in range(K)],
               edgecolor="white", width=0.6)
        ax.set_xticks(ks); ax.set_xticklabels([f"C{k}" for k in range(K)])
        ax.set_ylabel("Campioni assegnati"); ax.set_title(title, fontweight="bold")
        for k in range(K):
            ax.text(k, r["counts"][k]+2, f"{int(r['counts'][k])}",
                    ha="center", fontsize=9)

    def bar_thresholds(ax, r, K, palette, title):
        ks = np.arange(K); w = 0.25
        ax.bar(ks - w, r["dist_mean"], w, label="mean_dist",  color="#457b9d", alpha=0.85)
        ax.bar(ks,     r["dist_mean"]+3*r["dist_sigma"], w,
               label="mean+3σ",    color="#e9c46a", alpha=0.85)
        ax.bar(ks + w, r["dist_threshold"], w, label="threshold",  color="#e63946", alpha=0.85)
        ax.set_xticks(ks); ax.set_xticklabels([f"C{k}" for k in range(K)])
        ax.set_ylabel("Distanza euclidea"); ax.set_title(title, fontweight="bold")
        ax.legend(fontsize=9)

    bar_counts(    axes[0,0], r4,  4,  PAL4,  "K=4  — Campioni per cluster")
    bar_counts(    axes[0,1], r10, 10, PAL10, "K=10 — Campioni per cluster")
    bar_thresholds(axes[1,0], r4,  4,  PAL4,  "K=4  — Soglie per cluster")
    bar_thresholds(axes[1,1], r10, 10, PAL10, "K=10 — Soglie per cluster")
    fig.tight_layout()
    fig.savefig(OUT+"fig5_stats_k4_vs_k10.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig5_stats_k4_vs_k10.png")

    # ── Fig 6: Distribuzioni distanze per cluster (K=10) ─────────────────────
    active = [k for k in range(10) if (r10["assign"]==k).sum() >= 5]
    ncols  = min(4, len(active))
    nrows  = (len(active) + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows, ncols, figsize=(14, 4*nrows))
    fig.suptitle("Distribuzione distanze per cluster (K=10)  —  soglie di allarme",
                 fontsize=13, fontweight="bold")
    all_axes = axes.flatten() if hasattr(axes, "flatten") else [axes]
    for idx, k in enumerate(active):
        ax  = all_axes[idx]
        dk  = r10["dists"][r10["assign"] == k]
        ax.hist(dk, bins=30, color=PAL10[k], alpha=0.75, edgecolor="white")
        ax.axvline(r10["dist_mean"][k],
                   color="navy",   lw=2,   ls="-",  label=f"mean={r10['dist_mean'][k]:.3f}")
        ax.axvline(r10["dist_mean"][k] + 3*r10["dist_sigma"][k],
                   color="orange", lw=1.5, ls="--", label=f"mean+3σ")
        ax.axvline(r10["dist_max"][k] * 1.1,
                   color="red",    lw=1.5, ls=":",  label=f"max×1.1")
        ax.axvline(r10["dist_threshold"][k],
                   color="black",  lw=2.5, ls="-",  label=f"thr={r10['dist_threshold'][k]:.3f}")
        ax.set_title(f"Cluster {k}  (n={len(dk)})", fontweight="bold")
        ax.set_xlabel("Distanza euclidea"); ax.set_ylabel("Count")
        ax.legend(fontsize=8)
    for idx in range(len(active), len(all_axes)):
        all_axes[idx].set_visible(False)
    fig.tight_layout()
    fig.savefig(OUT+"fig6_distance_distributions.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig6_distance_distributions.png")

    # ── Fig 7: FP rate vs SIGMA_MULT per K=4 e K=10 ──────────────────────────
    sigma_range = np.arange(0.5, 6.1, 0.2)

    def fp_curve(r):
        rates = []
        for sm in sigma_range:
            n_fp = sum(1 for k, d in zip(r["assign"], r["dists"])
                       if d > max(r["dist_mean"][k] + sm*r["dist_sigma"][k],
                                  r["dist_max"][k]*MAX_DIST_MARGIN, MIN_THRESHOLD))
            rates.append(100.0 * n_fp / len(r["dists"]))
        return rates

    fp4  = fp_curve(r4)
    fp10 = fp_curve(r10)

    fig, ax = plt.subplots(figsize=(11, 6))
    fig.suptitle("FP rate vs SIGMA_MULT  —  K=4 vs K=10\n"
                 "(calcolato sui dati di baseline)",
                 fontsize=13, fontweight="bold")
    ax.plot(sigma_range, fp4,  color="#e63946", lw=2.5, marker="o", ms=5, label="K=4")
    ax.plot(sigma_range, fp10, color="#457b9d", lw=2.5, marker="s", ms=5, label="K=10")
    ax.axvline(SIGMA_MULT, color="black", lw=1.5, ls="--", alpha=0.6,
               label=f"σ={SIGMA_MULT} (scelto)")
    ax.axhline(0, color="green", lw=1, ls="--", alpha=0.4)
    for fp_list, col, lbl in [(fp4,"#e63946","K=4"),(fp10,"#457b9d","K=10")]:
        idx = next((i for i, v in enumerate(fp_list) if v == 0.0), None)
        if idx is not None:
            ax.annotate(f"{lbl}: FP=0 da σ={sigma_range[idx]:.1f}",
                        xy=(sigma_range[idx], 0.05),
                        xytext=(sigma_range[idx]+0.4, max(fp_list)*0.25),
                        color=col, fontsize=10, fontweight="bold",
                        arrowprops=dict(arrowstyle="->", color=col, lw=1.5))
    ax.set_xlabel("SIGMA_MULT"); ax.set_ylabel("FP rate (%)")
    ax.legend(fontsize=11)
    ax.set_ylim(-0.5, max(max(fp4), max(fp10))*1.15 + 0.1)
    fig.tight_layout()
    fig.savefig(OUT+"fig7_fp_vs_sigma.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig7_fp_vs_sigma.png")

    # ── Fig 8: Novelty buffer fill nel tempo ──────────────────────────────────
    fig, ax = plt.subplots(figsize=(12, 5))
    fig.suptitle(f"Riempimento del Novelty Buffer nel tempo\n"
                 f"(NOVELTY_THRESHOLD={NOVELTY_THRESH})",
                 fontsize=13, fontweight="bold")
    times_h = np.arange(len(nov_fill)) * (SAMPLE_COUNT / SAMPLE_RATE_HZ) / 3600
    ax.plot(times_h, nov_fill, color="#457b9d", lw=1.8)
    ax.axhline(NOVELTY_BUF_SIZE, color="#e63946", lw=1.5, ls="--",
               label=f"Buffer pieno ({NOVELTY_BUF_SIZE})")
    ax.axhline(10, color="gray", lw=1, ls=":", label="Min per seeding (K=10)")
    ax.fill_between(times_h, nov_fill, alpha=0.2, color="#457b9d")
    ax.set_xlabel("Tempo [ore]"); ax.set_ylabel("Campioni nel buffer")
    ax.legend(fontsize=11)
    fig.tight_layout()
    fig.savefig(OUT+"fig8_novelty_buffer.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig8_novelty_buffer.png")

    # ── Fig 9: Heatmap correlazione feature (top 20 più variabili) ────────────
    top20_idx  = np.argsort(features.std(axis=0))[-20:]
    corr       = np.corrcoef(features[:, top20_idx].T)
    names_top20 = [FEAT_NAMES[i] for i in top20_idx]

    fig, ax = plt.subplots(figsize=(13, 11))
    fig.suptitle("Matrice di correlazione — 20 feature più variabili",
                 fontsize=13, fontweight="bold")
    im = ax.imshow(corr, cmap="RdBu_r", vmin=-1, vmax=1, aspect="auto")
    ax.set_xticks(range(20)); ax.set_yticks(range(20))
    ax.set_xticklabels(names_top20, rotation=45, ha="right", fontsize=9)
    ax.set_yticklabels(names_top20, fontsize=9)
    plt.colorbar(im, ax=ax, label="Pearson r")
    for i in range(20):
        for j in range(20):
            ax.text(j, i, f"{corr[i,j]:.1f}", ha="center", va="center",
                    fontsize=6.5,
                    color="white" if abs(corr[i,j]) > 0.6 else "black")
    fig.tight_layout()
    fig.savefig(OUT+"fig9_feature_correlation.png", dpi=150, bbox_inches="tight")
    plt.close(); print("  fig9_feature_correlation.png")


    # ══════════════════════════════════════════════════════════════════════════
    #  SEZIONE COMPARATIVA  (solo se --csv2 è fornito)
    # ══════════════════════════════════════════════════════════════════════════
    if args.csv2:
        print("\n" + "═" * 60)
        print(f"  ANALISI COMPARATIVA: {args.label1}  vs  {args.label2}")
        print("═" * 60)

        def _load_env(csv_path, label):
            print(f"\n  [{label}] Caricamento...", flush=True)
            has_hdr = open(csv_path).readline().startswith("millis")
            df2 = pd.read_csv(csv_path, header=0 if has_hdr else None,
                              names=["millis","x","y","z"])
            dur_h = (df2.millis.iloc[-1] - df2.millis.iloc[0]) / 3_600_000
            print(f"  [{label}] {len(df2):,} campioni  |  {dur_h:.2f}h")
            ds2 = int(round(RAW_RATE_HZ / SAMPLE_RATE_HZ))
            lx = df2["x"].values[::ds2].astype(np.float32)
            ly = df2["y"].values[::ds2].astype(np.float32)
            lz = df2["z"].values[::ds2].astype(np.float32)
            lt = df2["millis"].values[::ds2].astype(np.float64)
            del df2; import gc; gc.collect()
            n_win2 = len(lx) // SAMPLE_COUNT
            print(f"  [{label}] Feature extraction su {n_win2:,} finestre...", flush=True)
            flist = []
            for i in range(n_win2):
                s2=i*SAMPLE_COUNT; e2=s2+SAMPLE_COUNT
                flist.append(compute_features(lx[s2:e2],ly[s2:e2],lz[s2:e2],lt[s2:e2]))
                if i % 2000 == 0 and i > 0:
                    print(f"    {i}/{n_win2}  ({i/n_win2*100:.0f}%)", flush=True)
            feats2 = np.array(flist)
            fm2=feats2.mean(0); fs2=feats2.std(0); fs2[fs2<1e-9]=1.0
            fn2=(feats2-fm2)/fs2
            buf2=[]; fill2=[]
            for fv2 in fn2:
                if len(buf2)<KMEANS_K: buf2.append(fv2.copy())
                else:
                    d2=np.linalg.norm(np.array(buf2)-fv2,axis=1)
                    ni2,nd2=d2.argmin(),d2.min()
                    if nd2>=NOVELTY_THRESH:
                        if len(buf2)<NOVELTY_BUF_SIZE: buf2.append(fv2.copy())
                        else: buf2[ni2]=fv2.copy()
                fill2.append(len(buf2))
            nov2=np.array(buf2)
            print(f"  [{label}] Novelty: {len(nov2)}/{NOVELTY_BUF_SIZE}  K-Means...", flush=True)
            r4_2=run_kmeans(fn2,nov2,4); r10_2=run_kmeans(fn2,nov2,10)
            return dict(label=label, feats=feats2, fnorm=fn2,
                        raw_x=lx, raw_y=ly, raw_z=lz,
                        nov_fill=np.array(fill2), dur_h=dur_h,
                        r4=r4_2, r10=r10_2)

        d1 = _load_env(args.csv,  args.label1)
        d2 = _load_env(args.csv2, args.label2)

        C1, C2 = "#e63946", "#457b9d"
        sr_cmp  = np.arange(0.5, 6.1, 0.2)

        def _fp(r):
            out=[]
            for sm in sr_cmp:
                n_fp=sum(1 for k,dd in zip(r["assign"],r["dists"])
                         if dd>max(r["dist_mean"][k]+sm*r["dist_sigma"][k],
                                   r["dist_max"][k]*MAX_DIST_MARGIN, MIN_THRESHOLD))
                out.append(100.*n_fp/len(r["dists"]))
            return out

        # cmp1 — PCA K=10 side-by-side
        fig,axes2=plt.subplots(1,2,figsize=(18,8))
        fig.suptitle(f"Spazio delle feature — K=10\n{args.label1}  vs  {args.label2}  —  PCA 2D",
                     fontsize=14,fontweight="bold")
        for ax2,(dd,col) in zip(axes2,[(d1,C1),(d2,C2)]):
            fn2=dd["fnorm"]; r10_=dd["r10"]
            pca2=PCA(n_components=2); pr2=pca2.fit_transform(fn2)
            cp2=pca2.transform(r10_["centroids"]); v2=pca2.explained_variance_ratio_
            for k in range(KMEANS_K):
                mk=r10_["assign"]==k
                ax2.scatter(pr2[mk,0],pr2[mk,1],color=PAL10[k],alpha=0.20,s=8,
                            label=f"C{k} n={mk.sum()}")
                if mk.sum()>=3: draw_ellipse(ax2,pr2[mk],cp2[k],PAL10[k])
            ax2.scatter(cp2[:,0],cp2[:,1],s=250,marker="*",zorder=6,
                        c=[PAL10[k] for k in range(KMEANS_K)],
                        edgecolors="black",linewidths=1.2)
            for k in range(KMEANS_K):
                ax2.annotate(f"C{k}",cp2[k],textcoords="offset points",
                             xytext=(5,4),fontsize=9,fontweight="bold",color=PAL10[k])
            deg2=sum(1 for k in range(KMEANS_K) if r10_["counts"][k]<=2)
            ax2.set_title(f"{dd['label']}  ({dd['dur_h']:.2f}h)\n"
                          f"var: {sum(v2)*100:.1f}%  |  degeneri: {deg2}/{KMEANS_K}",
                          fontsize=12,fontweight="bold")
            ax2.set_xlabel(f"PC1 ({v2[0]*100:.1f}%)")
            ax2.set_ylabel(f"PC2 ({v2[1]*100:.1f}%)")
            ax2.legend(fontsize=7,ncol=2,loc="upper right",markerscale=2)
        fig.tight_layout()
        fig.savefig(OUT+"cmp1_pca.png",dpi=150,bbox_inches="tight")
        plt.close(); print("  cmp1_pca.png")

        # cmp2 — Distribuzione campioni K=4 e K=10
        fig,axes2=plt.subplots(2,2,figsize=(16,10))
        fig.suptitle(f"Distribuzione campioni per cluster\nK=4 e K=10  —  {args.label1} vs {args.label2}",
                     fontsize=14,fontweight="bold")
        for ax2,(dd,K,pal,rkey) in zip(axes2.flatten(),
            [(d1,4,PAL4,"r4"),(d2,4,PAL4,"r4"),(d1,10,PAL10,"r10"),(d2,10,PAL10,"r10")]):
            r_=dd[rkey]; cnt_=r_["counts"]; n_t=cnt_.sum()
            xs2=np.arange(K)
            bars2=ax2.bar(xs2,cnt_,color=[pal[k] for k in range(K)],edgecolor="white",width=0.7)
            for k,(bar,cnt) in enumerate(zip(bars2,cnt_)):
                pct=cnt/n_t*100
                ax2.text(bar.get_x()+bar.get_width()/2,
                         bar.get_height()*0.5 if pct>5 else bar.get_height()+n_t*0.002,
                         f"{int(cnt)}\n({pct:.1f}%)",
                         ha="center",va="center" if pct>5 else "bottom",
                         fontsize=8,fontweight="bold",color="white" if pct>15 else "black")
                if cnt<=2: ax2.axvline(k,color="red",lw=2,alpha=0.4,ls="--")
            deg2=sum(1 for k in range(K) if cnt_[k]<=2)
            K_lbl="K=4" if K==4 else "K=10"
            ax2.set_title(f"{dd['label']} — {K_lbl}  (degeneri: {deg2}/{K})",fontweight="bold")
            ax2.set_xticks(xs2); ax2.set_xticklabels([f"C{k}" for k in range(K)])
            ax2.set_ylabel("Finestre assegnate")
        fig.tight_layout()
        fig.savefig(OUT+"cmp2_cluster_distribution.png",dpi=150,bbox_inches="tight")
        plt.close(); print("  cmp2_cluster_distribution.png")

        # cmp3 — Soglie K=10
        fig,axes2=plt.subplots(1,2,figsize=(16,6))
        fig.suptitle(f"Soglie di allarme per cluster (K=10)\n{args.label1} vs {args.label2}",
                     fontsize=14,fontweight="bold")
        for ax2,dd in zip(axes2,[d1,d2]):
            r10_=dd["r10"]; xs2=np.arange(KMEANS_K); w2=0.25
            ax2.bar(xs2-w2,r10_["dist_mean"],w2,label="mean_dist",color="#457b9d",alpha=0.85)
            ax2.bar(xs2,   r10_["dist_mean"]+3*r10_["dist_sigma"],w2,
                    label="mean+3σ",color="#e9c46a",alpha=0.85)
            ax2.bar(xs2+w2,r10_["thr"],w2,label="threshold",color="#e63946",alpha=0.85)
            for k in range(KMEANS_K):
                if r10_["counts"][k]<=2:
                    ax2.axvspan(k-0.5,k+0.5,alpha=0.08,color="red")
                    ax2.text(k,float(r10_["thr"].max())*0.95,"DEG",
                             ha="center",fontsize=8,color="red",fontweight="bold")
            ax2.set_xticks(xs2); ax2.set_xticklabels([f"C{k}" for k in range(KMEANS_K)])
            ax2.set_title(dd["label"],fontweight="bold")
            ax2.set_ylabel("Distanza euclidea"); ax2.legend(fontsize=9)
        fig.tight_layout()
        fig.savefig(OUT+"cmp3_thresholds.png",dpi=150,bbox_inches="tight")
        plt.close(); print("  cmp3_thresholds.png")

        # cmp4 — PSD media per asse
        def _psd(rx,ry,rz,n=500):
            nw=len(rx)//SAMPLE_COUNT; step=max(1,nw//n)
            px,py,pz,pm=[],[],[],[]
            for i in range(0,min(nw,n*step),step):
                s2=i*SAMPLE_COUNT; e2=s2+SAMPLE_COUNT; h=np.hamming(SAMPLE_COUNT)
                px.append(np.abs(np.fft.rfft(rx[s2:e2]*h))[1:])
                py.append(np.abs(np.fft.rfft(ry[s2:e2]*h))[1:])
                pz.append(np.abs(np.fft.rfft(rz[s2:e2]*h))[1:])
                m2=np.sqrt(rx[s2:e2]**2+ry[s2:e2]**2+rz[s2:e2]**2)
                pm.append(np.abs(np.fft.rfft(m2*h))[1:])
            return np.mean(px,0),np.mean(py,0),np.mean(pz,0),np.mean(pm,0)

        freqs2=np.fft.rfftfreq(SAMPLE_COUNT,1./SAMPLE_RATE_HZ)[1:]
        ps1=_psd(d1["raw_x"],d1["raw_y"],d1["raw_z"])
        ps2=_psd(d2["raw_x"],d2["raw_y"],d2["raw_z"])
        fig,axes2=plt.subplots(2,2,figsize=(16,10),sharex=True)
        fig.suptitle(f"Profilo spettrale medio per asse\n{args.label1} vs {args.label2}",
                     fontsize=14,fontweight="bold")
        for ax2,(aname,p1_,p2_) in zip(axes2.flatten(),
                                        zip(["X","Y","Z","|XYZ|"],ps1,ps2)):
            ax2.fill_between(freqs2,p1_,alpha=0.25,color=C1)
            ax2.plot(freqs2,p1_,color=C1,lw=1.8,label=args.label1)
            ax2.fill_between(freqs2,p2_,alpha=0.25,color=C2)
            ax2.plot(freqs2,p2_,color=C2,lw=1.8,label=args.label2)
            for lo,hi,bn in [(1,5,"1-5Hz"),(5,20,"5-20Hz"),(20,40,"20-40Hz")]:
                ax2.axvspan(lo,hi,alpha=0.08,color="gray")
            ax2.set_title(f"Asse {aname}",fontweight="bold")
            ax2.set_xlabel("Frequenza [Hz]"); ax2.set_ylabel("Magn. FFT media")
            ax2.legend(fontsize=10); ax2.set_xlim(0,50)
        fig.tight_layout()
        fig.savefig(OUT+"cmp4_psd.png",dpi=150,bbox_inches="tight")
        plt.close(); print("  cmp4_psd.png")

        # cmp5 — Violin plot feature chiave
        fig,axes2=plt.subplots(2,3,figsize=(16,10))
        fig.suptitle(f"Distribuzione feature chiave\n{args.label1} vs {args.label2}",
                     fontsize=14,fontweight="bold")
        for ax2,(fi,fname) in zip(axes2.flatten(),
            [(0,"impact_score"),(1,"m_p99"),(5,"m_jerk_max"),
             (9,"m_band_20_40"),(13,"m_band_1_5"),(21,"x_zcr")]):
            vr=d1["feats"][:,fi]; vc=d2["feats"][:,fi]
            lo=min(np.percentile(vr,0.5),np.percentile(vc,0.5))
            hi=max(np.percentile(vr,99.5),np.percentile(vc,99.5))
            vrc=vr[(vr>=lo)&(vr<=hi)]; vcc=vc[(vc>=lo)&(vc<=hi)]
            vp=ax2.violinplot([vrc,vcc],positions=[1,2],
                              showmedians=True,showextrema=False,widths=0.7)
            vp["bodies"][0].set_facecolor(C1); vp["bodies"][0].set_alpha(0.6)
            vp["bodies"][1].set_facecolor(C2); vp["bodies"][1].set_alpha(0.6)
            vp["cmedians"].set_color("black"); vp["cmedians"].set_lw(2)
            ax2.set_xticks([1,2]); ax2.set_xticklabels([args.label1,args.label2])
            ax2.set_title(fname,fontweight="bold"); ax2.set_ylabel("Valore feature")
            for pos,v,col in [(1,vr,C1),(2,vc,C2)]:
                ax2.text(pos,np.median(v),f" {np.median(v):.3f}",va="bottom",
                         fontsize=9,color=col,fontweight="bold")
        fig.tight_layout()
        fig.savefig(OUT+"cmp5_feature_distributions.png",dpi=150,bbox_inches="tight")
        plt.close(); print("  cmp5_feature_distributions.png")

        # cmp6 — Novelty buffer fill
        fig,ax2=plt.subplots(figsize=(13,6))
        fig.suptitle(f"Riempimento Novelty Buffer\n{args.label1} vs {args.label2}",
                     fontsize=14,fontweight="bold")
        for dd,col in [(d1,C1),(d2,C2)]:
            fill2=dd["nov_fill"]
            th=np.arange(len(fill2))*(SAMPLE_COUNT/SAMPLE_RATE_HZ)/3600
            ax2.plot(th,fill2,color=col,lw=2,label=dd["label"])
            ax2.fill_between(th,fill2,alpha=0.12,color=col)
            fi2=np.argmax(fill2>=NOVELTY_BUF_SIZE)
            if fi2>0:
                ax2.annotate(f"{dd['label']}: pieno\na {th[fi2]:.2f}h",
                             xy=(th[fi2],NOVELTY_BUF_SIZE),
                             xytext=(th[fi2]+0.2,NOVELTY_BUF_SIZE-20),
                             color=col,fontsize=10,fontweight="bold",
                             arrowprops=dict(arrowstyle="->",color=col,lw=1.5))
        ax2.axhline(NOVELTY_BUF_SIZE,color="black",lw=1.5,ls="--",alpha=0.5,
                    label=f"Pieno ({NOVELTY_BUF_SIZE})")
        ax2.axhline(KMEANS_K,color="gray",lw=1,ls=":",alpha=0.5,
                    label=f"Min seeding (K={KMEANS_K})")
        ax2.set_xlabel("Tempo [ore]"); ax2.set_ylabel("Campioni nel buffer")
        ax2.legend(fontsize=11)
        fig.tight_layout()
        fig.savefig(OUT+"cmp6_novelty_fill.png",dpi=150,bbox_inches="tight")
        plt.close(); print("  cmp6_novelty_fill.png")

        # cmp7 — FP vs sigma
        fig,axes2=plt.subplots(1,2,figsize=(16,6),sharey=True)
        fig.suptitle(f"FP rate vs SIGMA_MULT\n{args.label1} vs {args.label2}",
                     fontsize=14,fontweight="bold")
        for ax2,(dd,ttl) in zip(axes2,[(d1,args.label1),(d2,args.label2)]):
            fp_k4=_fp(dd["r4"]); fp_k10=_fp(dd["r10"])
            ax2.plot(sr_cmp,fp_k4, color="#e63946",lw=2.5,marker="o",ms=4,label="K=4")
            ax2.plot(sr_cmp,fp_k10,color="#457b9d",lw=2.5,marker="s",ms=4,label="K=10")
            ax2.axvline(SIGMA_MULT,color="black",lw=1.5,ls="--",alpha=0.5,
                        label=f"σ={SIGMA_MULT} (scelto)")
            ax2.axhline(0,color="green",lw=1,ls="--",alpha=0.4)
            for fp_l,col,lbl in [(fp_k4,"#e63946","K=4"),(fp_k10,"#457b9d","K=10")]:
                idx2=next((i for i,v in enumerate(fp_l) if v==0.),None)
                if idx2 is not None:
                    ax2.annotate(f"{lbl}: FP=0\nda σ={sr_cmp[idx2]:.1f}",
                                 xy=(sr_cmp[idx2],0.05),
                                 xytext=(sr_cmp[idx2]+0.4,max(fp_l)*0.3),
                                 color=col,fontsize=9,fontweight="bold",
                                 arrowprops=dict(arrowstyle="->",color=col,lw=1.5))
            ax2.set_title(ttl,fontweight="bold")
            ax2.set_xlabel("SIGMA_MULT"); ax2.set_ylabel("FP rate (%)")
            ax2.legend(fontsize=10)
            ax2.set_ylim(-0.5,max(max(fp_k4),max(fp_k10))*1.15+0.1)
        fig.tight_layout()
        fig.savefig(OUT+"cmp7_fp_vs_sigma.png",dpi=150,bbox_inches="tight")
        plt.close(); print("  cmp7_fp_vs_sigma.png")

        print(f"\n  Grafici comparativi salvati in: {os.path.abspath(OUT)}")
        print("═" * 60)
    else:
        print("\n  (Nessun --csv2: analisi comparativa saltata)")

    print(f"\n  Tutti i grafici salvati in: {os.path.abspath(OUT)}")
    print("═" * 60)


if __name__ == "__main__":
    main()
