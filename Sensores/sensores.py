import random

class Sensores:
    def __init__(self):
        # Inicialización de estados base (puntos de partida)
        self.temp_actual = 45.0
        self.cons_actual = 5.0
        self.vibr_actual = 2.0
        self.hum_actual = 50.0
        self.estado_actual = "Operativo"
        self.estados_posibles = ["Operativo", "Mantenimiento", "Falla Crítica", "Standby"]

    def temperature(self):
        # Fluctuación suave de ±0.5 grados
        variacion = random.uniform(-0.5, 0.5)
        self.temp_actual = max(18.0, min(95.0, self.temp_actual + variacion))
        return round(self.temp_actual, 2)

    def energy_consumption(self):
        # El consumo fluctúa según el estado; si está en falla, baja drásticamente
        if self.estado_actual in ["Falla Crítica", "Standby"]:
            self.cons_actual = max(0.1, self.cons_actual - 0.5)
        else:
            variacion = random.uniform(-0.2, 0.2)
            self.cons_actual = max(0.5, min(20.0, self.cons_actual + variacion))
        return round(self.cons_actual, 3)

    def mechanical_vibration(self):
        # La vibración tiende a mantenerse estable a menos que haya un "pico"
        variacion = random.uniform(-0.1, 0.1)
        self.vibr_actual = max(0.0, min(12.0, self.vibr_actual + variacion))
        return round(self.vibr_actual, 2)

    def humidity(self):
        # Fluctuación ambiental lenta de ±0.2%
        variacion = random.uniform(-0.2, 0.2)
        self.hum_actual = max(20.0, min(95.0, self.hum_actual + variacion))
        return round(self.hum_actual, 1)

    def operational_status(self):
        # El estado solo cambia con una probabilidad muy baja (mantiene consistencia)
        if random.random() < 0.05:  # 5% de probabilidad de cambiar de estado
            self.estado_actual = random.choices(self.estados_posibles, weights=[85, 8, 2, 5])[0]
        return self.estado_actual