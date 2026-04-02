"""
iot_simulator.py — Generador de valores de sensores IoT
Contiene la clase SensorModel con lógica de simulación realista.
Es importada por iot_clients.py, que maneja la red.
"""

import random


class SensorModel:
    """
    Modela el comportamiento físico de un sensor IoT.
    Cada instancia representa un sensor individual con su propio
    estado interno que evoluciona gradualmente (random walk suave).
    """

    # Configuración de cada tipo de sensor:
    # tipo → (método de lectura, unidad, umbral de alerta)
    TIPOS = {
        "TEMP": ("temperatura",   "°C",    80.0),
        "VIBR": ("vibracion",     "mm/s",  10.0),
        "ENER": ("energia",       "kWh",   18.0),
        "HUM":  ("humedad",       "%",     90.0),
        "STAT": ("estado_operativo", "",    None),
    }

    def __init__(self, sensor_id: str, tipo: str):
        if tipo not in self.TIPOS:
            raise ValueError(f"Tipo desconocido: {tipo}. Válidos: {list(self.TIPOS)}")

        self.sensor_id = sensor_id
        self.tipo      = tipo
        _, self.unidad, self.umbral = self.TIPOS[tipo]

        # Estado interno inicial según el tipo
        self._temp   = 45.0
        self._energy = 5.0
        self._vibr   = 2.0
        self._hum    = 50.0
        self._estado = "Operativo"
        # Sin acentos: evita problemas de encoding UTF-8 al transmitir por socket
        self._estados_posibles = ["Operativo", "Mantenimiento", "Falla_Critica", "Standby"]

    # ── Métodos privados de simulación ────────────────────────────

    def _temperatura(self) -> float:
        variacion = random.uniform(-0.5, 0.5)
        self._temp = max(18.0, min(95.0, self._temp + variacion))
        return round(self._temp, 2)

    def _vibracion(self) -> float:
        variacion = random.uniform(-0.1, 0.1)
        self._vibr = max(0.0, min(12.0, self._vibr + variacion))
        return round(self._vibr, 2)

    def _energia(self) -> float:
        if self._estado in ("Falla_Critica", "Standby"):
            self._energy = max(0.1, self._energy - 0.5)
        else:
            variacion = random.uniform(-0.2, 0.2)
            self._energy = max(0.5, min(20.0, self._energy + variacion))
        return round(self._energy, 3)

    def _humedad(self) -> float:
        variacion = random.uniform(-0.2, 0.2)
        self._hum = max(20.0, min(95.0, self._hum + variacion))
        return round(self._hum, 1)

    def _estado_operativo(self) -> str:
        if random.random() < 0.05:
            self._estado = random.choices(
                self._estados_posibles, weights=[85, 8, 2, 5]
            )[0]
        return self._estado

    # ── API pública ────────────────────────────────────────────────

    def leer(self) -> str:
        """Devuelve el valor actual del sensor como string."""
        metodo_nombre, _, _ = self.TIPOS[self.tipo]
        metodo = getattr(self, f"_{metodo_nombre}")
        return str(metodo())

    def esta_en_alerta(self, valor_str: str) -> bool:
        """Indica si el valor supera el umbral de alerta."""
        if self.umbral is None:
            return False
        try:
            return float(valor_str) > self.umbral
        except ValueError:
            return False