import random

class Sensores:
    def __init__(self):
        self.estados = ["Operativo", "Mantenimiento", "Falla Crítica", "Standby"]

    def temperature(self):
        """Simula temperatura en grados Celsius (rango 18°C a 85°C)."""
        return round(random.uniform(18.0, 85.0), 2)

    def energy_consumption(self):
        """Simula consumo en kWh (rango 0.5 a 15.0)."""
        return round(random.uniform(0.5, 15.0), 3)

    def mechanical_vibration(self):
        """Simula vibración en mm/s (rango 0 a 10)."""
        return round(random.uniform(0.0, 10.0), 2)

    def humidity(self):
        """Simula humedad relativa en porcentaje (30% a 90%)."""
        return round(random.uniform(30.0, 90.0), 1)

    def operational_status(self):
        """Devuelve un estado aleatorio basado en pesos de probabilidad."""
        # Se asume que la mayor parte del tiempo el equipo está operativo
        return random.choices(self.estados, weights=[90, 5, 2.5, 2.5])[0]