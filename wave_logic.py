enemycolor = {
    "Rot" : 1,
    "Blau" : 3,
    "Schwarz" : 5,
    "Silber" : 7,
    "Gold" : 10,
}


enemytype = {
    "Boko" : 1,
    "Echse" : 1.5,
    "Moblin" : 2,
    "Leune" : 10,
    "Hynox" 20
}

enemyweapon = {
    "swordshield" : {
        "Boko_1" : 1,
        "Boko_2" : 1.5,
        "Echse_1" : 1.25,
        "Echse_2" : 2,
        "Soldier" : 2,
        "Ritter" : 3,
        "Royal" : 5,
        "Guard" : 7,
        "Ice&Royal" : 6,
        "Ele&Royal" : 7
    },
    "bow" : {
        "Boko_1" : 1,
        "Boko_2" : 1.5,
        "Echse_1" : 1.25,
        "Echse_2" : 2,
        "Soldier" : 2,
        "Ritter" : 3,
        "Leune_1" : 4,
        "Leune_2" : 6,
        "Leune_3" : 8,
        "Royal" : 5,
        "Guard" : 7
    },
    "arroweffect" : {
        "Ele" : 2,
        "Nor" : 1,
        "Ice" : 2,
        "Bomb" : 1,75,
        "Fire" :  1.5,
        "Ancient" : 4
    }
}


scale = enemycolor * enemytype + enemyweapon

Besonderheiten: 
Hynox hat keine Waffen
Leune hat von jeder waffe eins (swordshield, bow, arrow)
Boko, Moblin, Echse Hhat nur entweder swordshield oder bow und arrow