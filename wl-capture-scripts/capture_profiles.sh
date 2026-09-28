# Liste di canali condivise fra capture_hot_init.sh e capture_cold_init.sh.
# Va copiato sul device accanto a loro: entrambi lo cercano nella directory da
# cui sono stati lanciati e si fermano se non lo trovano, invece di proseguire
# con liste diverse a seconda dello script.
#
# I gruppi sono DISGIUNTI. Un canale con guardia radar sta nel gruppo dfs o in
# quello meteo e in nessun altro: metterlo anche nel gruppo normale significa
# catturarlo due volte e, la prima, aspettare il CAC con un SETTLE tarato per
# un canale che non ce l'ha, cioe' una cattura che non sale al BSS.
#
#   <bw>        i canali SENZA guardia radar di quella larghezza
#   <bw>dfs     quelli con guardia radar, CAC ~60 s: SETTLE=70
#   <bw>meteo   i 5600-5650 del radar meteo, CAC 600 s: SETTLE=620
#
# Niente head/awk/sed: il busybox di questi firmware non li ha tutti.
#
# NOTA su 144: qui sta fra i non-DFS perche' e' cosi' che era classificato
# quando le liste sono nate. Sotto FCC ha la guardia radar e sotto ETSI non e'
# allocato affatto. Se `up` su 5g144 resta appeso una sessantina di secondi,
# per il dominio di questa board va spostato in C20DFS.

C20="36 40 44 48 144 149 153 157 161 165"
C20DFS="52 56 60 64 100 104 108 112 116 132 136 140"
C20METEO="120 124 128"

# A 40 e 80 MHz il driver vuole il CANALE BASSO del blocco, non quello
# centrale: 36/40 e non 38/40, 36/80 e non 42/80. Col centrale rifiuta.
C40="36 44 149 157"
C40DFS="52 60 100 108 132 140"
C40METEO="116 124"

C80="36 149"
C80DFS="52 100 132"
C80METEO="116"

PROFILI="20 40 80 20dfs 40dfs 80dfs 20meteo 40meteo 80meteo"

# profilo <nome>: imposta LIST e BW, ritorna 1 se il nome non e' un profilo.
profilo() {
    case "$1" in
        20)      LIST="$C20";      BW=20 ;;
        40)      LIST="$C40";      BW=40 ;;
        80)      LIST="$C80";      BW=80 ;;
        20dfs)   LIST="$C20DFS";   BW=20 ;;
        40dfs)   LIST="$C40DFS";   BW=40 ;;
        80dfs)   LIST="$C80DFS";   BW=80 ;;
        20meteo) LIST="$C20METEO"; BW=20 ;;
        40meteo) LIST="$C40METEO"; BW=40 ;;
        80meteo) LIST="$C80METEO"; BW=80 ;;
        *)       return 1 ;;
    esac
    return 0
}
